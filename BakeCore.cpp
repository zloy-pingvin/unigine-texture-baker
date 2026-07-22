#include "BakeCore.h"
#include "BakeGpu.h"

#include <UnigineEngine.h>
#include <UnigineFileSystem.h>
#include <UnigineImage.h>
#include <UnigineLog.h>
#include <UnigineMaterial.h>
#include <UnigineMaterials.h>
#include <UnigineMathLib.h>
#include <UnigineMesh.h>
#include <UnigineXml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace Unigine;
using namespace Unigine::Math;

namespace BakeCore
{

namespace
{

//------------------------------------------------------------------------------
// Path helpers (virtual, forward-slash paths).
//------------------------------------------------------------------------------

// Resolves any engine path (including "guid:/..." runtime references) to a
// human-readable virtual asset path like "models/rock.fbx".
String resolveAssetPath(const char *path)
{
	if (String::isEmpty(path))
		return String();
	UGUID guid = FileSystem::getGUID(path);
	if (guid.isValid())
	{
		UGUID asset = FileSystemAssets::resolveAsset(guid);
		String vp = FileSystem::getVirtualPath(asset.isValid() ? asset : guid);
		if (!vp.empty())
			return vp;
	}
	return String(path);
}

bool isGuidPath(const String &path)
{
	return path.size() >= 6 && !strncmp(path.get(), "guid:/", 6);
}

String pathDir(const String &path)
{
	int pos = -1;
	for (int i = 0; i < path.size(); i++)
		if (path[i] == '/' || path[i] == '\\')
			pos = i;
	if (pos < 0)
		return String("");
	return path.substr(0, pos + 1);
}

String pathBaseName(const String &path)
{
	int slash = -1;
	for (int i = 0; i < path.size(); i++)
		if (path[i] == '/' || path[i] == '\\')
			slash = i;
	String name = path.substr(slash + 1);
	int dot = -1;
	for (int i = 0; i < name.size(); i++)
		if (name[i] == '.')
			dot = i;
	if (dot > 0)
		name = name.substr(0, dot);
	return name;
}

//------------------------------------------------------------------------------
// Source (high-poly) data.
//------------------------------------------------------------------------------

struct SourceSurface
{
	ImagePtr albedo;   // may be null
	ImagePtr shading;  // may be null
	ImagePtr normal;   // may be null
	ImagePtr emission; // may be null

	// per-tvertex attributes: world-space normal/tangent decoded from the mesh
	// quaternion basis per vertex, then interpolated linearly across triangles —
	// exactly like the engine passes them as varyings (see mesh vertex.h)
	std::vector<vec2> uv;
	std::vector<vec2> uv1; // second UV set (unique unwrap) — GPU captures unwrap in it
	std::vector<vec3> n, t;
	std::vector<float> sw; // sign of the quat w = binormal orientation
	bool hasUV = false;
	bool hasUV1 = false;
	bool normalInvertG = false;          // import setting of the source normal map
	vec4 uvTransform{1.0f, 1.0f, 0.0f, 0.0f}; // material base UV tiling: uv * xy + zw
	// material constants (used as fallback when textures are missing)
	vec4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
	float metalness = 0.0f;
	float roughness = 1.0f;
	bool emissionOn = false; // material "emission" state
	vec4 emissionColor{1.0f, 1.0f, 1.0f, 1.0f};
	float emissionScale = 1.0f;

	// GPU "as rendered" capture of this surface (unwrap gbuffer render)
	BakeGpu::SurfaceCapture gpu;
	bool useGpu = false;
};

struct SourceTri
{
	vec3 p0, e1, e2; // world-space vertex + edges (Moller-Trumbore)
	int surface;
	int t0, t1, t2; // tvertex indices
	int corner;     // base corner slot (k*3) in the surface's index order — GPU atlasUV lookup
};

//------------------------------------------------------------------------------
// BVH.
//------------------------------------------------------------------------------

struct BVHNode
{
	vec3 bmin, bmax;
	int left = -1;  // inner: child index; leaf: first triangle
	int count = 0;  // leaf: triangle count; inner: 0
	int right = -1; // inner: child index
};

class BVH
{
public:
	void build(const std::vector<SourceTri> &tris)
	{
		int n = int(tris.size());
		order.resize(n);
		centroids.resize(n);
		boundsMin.resize(n);
		boundsMax.resize(n);
		for (int i = 0; i < n; i++)
		{
			order[i] = i;
			const SourceTri &tr = tris[i];
			vec3 p1 = tr.p0 + tr.e1;
			vec3 p2 = tr.p0 + tr.e2;
			vec3 bmin = min(tr.p0, min(p1, p2));
			vec3 bmax = max(tr.p0, max(p1, p2));
			boundsMin[i] = bmin;
			boundsMax[i] = bmax;
			centroids[i] = (bmin + bmax) * 0.5f;
		}
		nodes.clear();
		nodes.reserve(size_t(n) * 2 + 1);
		if (n > 0)
			buildNode(0, n);
		centroids.clear();
		centroids.shrink_to_fit();
	}

	// Collects the hit with the lowest score; the scorer gets (triangle index, t, u, v)
	// and returns the hit's score (lower is better). Returns triangle index or -1.
	template <typename Scorer>
	int trace(const std::vector<SourceTri> &tris, const vec3 &orig, const vec3 &dir,
		float tmax, const Scorer &scorer, float &outT, float &outU, float &outV) const
	{
		if (nodes.empty())
			return -1;
		vec3 invDir;
		for (int i = 0; i < 3; i++)
			invDir[i] = (Math::abs(dir[i]) > 1e-20f) ? 1.0f / dir[i] : 1e20f;

		int best = -1;
		float bestScore = 1e30f;
		int stack[64];
		int sp = 0;
		stack[sp++] = 0;
		while (sp > 0)
		{
			const BVHNode &node = nodes[stack[--sp]];
			if (!hitAABB(orig, invDir, node.bmin, node.bmax, tmax))
				continue;
			if (node.count > 0)
			{
				for (int i = 0; i < node.count; i++)
				{
					int tri = order[node.left + i];
					float t, u, v;
					if (intersectTri(orig, dir, tris[tri], tmax, t, u, v))
					{
						float score = scorer(tri, t, u, v);
						if (score < bestScore)
						{
							bestScore = score;
							best = tri;
							outT = t;
							outU = u;
							outV = v;
						}
					}
				}
			}
			else
			{
				if (sp < 62)
				{
					stack[sp++] = node.left;
					stack[sp++] = node.right;
				}
			}
		}
		return best;
	}

private:
	static bool hitAABB(const vec3 &o, const vec3 &invD, const vec3 &bmin, const vec3 &bmax, float tmax)
	{
		float t0 = 0.0f, t1 = tmax;
		for (int i = 0; i < 3; i++)
		{
			float tn = (bmin[i] - o[i]) * invD[i];
			float tf = (bmax[i] - o[i]) * invD[i];
			if (tn > tf)
			{
				float tmp = tn;
				tn = tf;
				tf = tmp;
			}
			t0 = Math::max(t0, tn);
			t1 = Math::min(t1, tf);
			if (t0 > t1)
				return false;
		}
		return true;
	}

	static bool intersectTri(const vec3 &orig, const vec3 &dir, const SourceTri &tr,
		float tmax, float &t, float &u, float &v)
	{
		vec3 pvec = cross(dir, tr.e2);
		float det = dot(tr.e1, pvec);
		if (Math::abs(det) < 1e-12f)
			return false;
		float inv = 1.0f / det;
		vec3 tvec = orig - tr.p0;
		u = dot(tvec, pvec) * inv;
		if (u < -1e-5f || u > 1.0f + 1e-5f)
			return false;
		vec3 qvec = cross(tvec, tr.e1);
		v = dot(dir, qvec) * inv;
		if (v < -1e-5f || u + v > 1.0f + 1e-5f)
			return false;
		t = dot(tr.e2, qvec) * inv;
		return t >= 0.0f && t <= tmax;
	}

	int buildNode(int start, int count)
	{
		int nodeIndex = int(nodes.size());
		nodes.push_back(BVHNode());

		vec3 bmin(1e30f), bmax(-1e30f);
		for (int i = start; i < start + count; i++)
		{
			bmin = min(bmin, boundsMin[order[i]]);
			bmax = max(bmax, boundsMax[order[i]]);
		}
		nodes[nodeIndex].bmin = bmin;
		nodes[nodeIndex].bmax = bmax;

		if (count <= 4)
		{
			nodes[nodeIndex].left = start;
			nodes[nodeIndex].count = count;
			return nodeIndex;
		}

		vec3 extent = bmax - bmin;
		int axis = 0;
		if (extent.y > extent.x)
			axis = 1;
		if (extent.z > extent[axis])
			axis = 2;

		int mid = start + count / 2;
		std::nth_element(order.begin() + start, order.begin() + mid, order.begin() + start + count,
			[this, axis](int a, int b) { return centroids[a][axis] < centroids[b][axis]; });

		int left = buildNode(start, mid - start);
		int right = buildNode(mid, start + count - mid);
		nodes[nodeIndex].left = left;
		nodes[nodeIndex].right = right;
		nodes[nodeIndex].count = 0;
		return nodeIndex;
	}

	std::vector<BVHNode> nodes;
	std::vector<int> order;
	std::vector<vec3> centroids;
	std::vector<vec3> boundsMin, boundsMax;
};

//------------------------------------------------------------------------------
// Target (low-poly) data.
//------------------------------------------------------------------------------

struct TargetTri
{
	vec3 p0, p1, p2;    // world-space positions
	vec3 n0, n1, n2;    // world-space shading normals
	vec3 t0, t1, t2;    // world-space tangents
	float signW;        // binormal orientation (sign of quat w)
	vec3 r0, r1, r2;    // world-space smoothed cage normals (ray directions)
	vec3 faceN;         // world-space geometric normal (skew-mask ray direction)
	vec2 uv0, uv1, uv2; // UV0
	int group = 0;      // rays trace only into this bake group's high-poly set
};

struct TBN
{
	vec3 t, b, n;
};

// Reproduces the engine's per-pixel normalizationTBN() (core/materials/shaders/api/matrix.h):
// normalize interpolated T and N, Gram-Schmidt T against N, B = cross(N, T) * sign.
TBN buildTBN(const vec3 &nInterp, const vec3 &tInterp, float signBinormal)
{
	TBN r;
	r.n = normalize(nInterp);
	vec3 t = normalize(tInterp);
	r.t = normalize(t - r.n * dot(t, r.n));
	r.b = normalize(cross(r.n, r.t)) * signBinormal;
	return r;
}

// Key for merging vertices by position (hard edges and surface borders share
// one smoothed cage normal, so rays do not split apart there).
struct QuantizedPos
{
	long long x, y, z;
	bool operator<(const QuantizedPos &o) const
	{
		if (x != o.x)
			return x < o.x;
		if (y != o.y)
			return y < o.y;
		return z < o.z;
	}
};

QuantizedPos quantize(const vec3 &p)
{
	const double q = 1.0 / 1e-5; // 0.01 mm cells
	return {std::llround(double(p.x) * q), std::llround(double(p.y) * q), std::llround(double(p.z) * q)};
}

//------------------------------------------------------------------------------
// Texture loading.
//------------------------------------------------------------------------------

ImagePtr loadTexture(const char *path, std::vector<std::pair<String, ImagePtr>> &cache)
{
	if (!path || !*path)
		return ImagePtr();
	for (const auto &entry : cache)
		if (entry.first == path)
			return entry.second;

	ImagePtr img = Image::create();
	if (!img->load(path))
	{
		Log::warning("Baker: failed to load texture \"%s\"\n", path);
		img = ImagePtr();
	}
	else
	{
		if (img->isCompressedFormat())
			img->decompress();
		Log::message("Baker: loaded texture \"%s\" (%dx%d, %d channels)\n",
			path, img->getWidth(), img->getHeight(), img->getNumChannels());
	}
	cache.push_back({String(path), img});
	return img;
}

// Reads the "invert_g" import parameter from the source asset's .meta file.
// Runtime ("guid://...") textures already have all import operations applied,
// so the flip must be respected only when the raw source file was loaded.
bool sourceNormalInvertG(const char *path)
{
	if (String::isEmpty(path) || !strncmp(path, "guid://", 7))
		return false;

	String metaPath = String(Engine::get()->getDataPath());
	if (!metaPath.empty() && metaPath[metaPath.size() - 1] != '/' && metaPath[metaPath.size() - 1] != '\\')
		metaPath += "/";
	metaPath += resolveAssetPath(path) + ".meta";

	XmlPtr xml = Xml::create();
	if (!xml->load(metaPath.get()))
		return false;
	XmlPtr params = xml->getChild("parameters");
	if (!params)
		return false;
	for (int i = 0; i < params->getNumChildren(); i++)
	{
		XmlPtr p = params->getChild(i);
		const char *argName = p ? p->getArg("name") : nullptr;
		if (argName && !strcmp(argName, "invert_g"))
			return p->getIntData() != 0;
	}
	return false;
}

} // namespace

//------------------------------------------------------------------------------
// Baking.
//------------------------------------------------------------------------------

Result bake(const std::vector<BakeGroup> &groups,
	const Settings &settings, const ProgressFn &progress)
{
	Result result;

	auto fail = [&result](const char *msg) -> Result & {
		result.success = false;
		result.error = msg;
		Log::error("Baker: %s\n", msg);
		return result;
	};

	if (groups.empty())
		return fail("Models are not set.");
	for (const BakeGroup &g : groups)
		if (g.highs.empty() || g.lows.empty())
			return fail("A bake group has no high-poly or no low-poly set.");

	const Ptr<ObjectMeshStatic> &firstHigh = groups[0].highs[0];
	const Ptr<ObjectMeshStatic> &firstLow = groups[0].lows[0];

	Log::message("Baker: bake start: resolution=%d frontal=%f rear=%f supersamples=%d groups=%d high=\"%s\" low=\"%s\"\n",
		settings.resolution, settings.frontalDistance, settings.rearDistance, settings.supersamples,
		int(groups.size()), firstHigh->getName(), firstLow->getName());

	if (!progress(0, "Reading geometry..."))
	{
		result.cancelled = true;
		return result;
	}

	//--------------------------------------------------------------------------
	// Extract high-poly geometry, materials and textures. Surfaces of all
	// groups and objects share one consecutive numbering (matches the capture
	// indexing). Triangles are kept per group: each group traces only its own.
	//--------------------------------------------------------------------------
	std::vector<SourceSurface> srcSurfaces;
	std::vector<std::vector<SourceTri>> groupTris(groups.size());
	std::vector<std::pair<String, ImagePtr>> textureCache;
	bool anyAlbedo = false, anyShading = false, anyNormalMap = false;

	for (size_t gi = 0; gi < groups.size(); gi++)
	{
	std::vector<SourceTri> &srcTris = groupTris[gi];
	for (const Ptr<ObjectMeshStatic> &high : groups[gi].highs)
	{
	Ptr<ConstMesh> highMesh = high->getMeshForceRAM();
	if (!highMesh)
	{
		Log::warning("Baker: cannot get geometry of \"%s\", skipping the object\n", high->getName());
		continue;
	}
	mat4 highTm = mat4(high->getWorldTransform());
	mat3 highNm = transpose(inverse(mat3(highTm)));
	const int surfBase = int(srcSurfaces.size());
	srcSurfaces.resize(surfBase + highMesh->getNumSurfaces());

	for (int ls = 0; ls < highMesh->getNumSurfaces(); ls++)
	{
		const int s = surfBase + ls; // flat surface index across all objects
		SourceSurface &surf = srcSurfaces[s];

		Log::message("Baker: high-poly \"%s\" surface %d \"%s\": %d triangles, enabled=%d, viewport_mask=0x%08x, lod=[%g..%g], material=\"%s\"\n",
			high->getName(), s, highMesh->getSurfaceName(ls), highMesh->getNumCIndices(ls) / 3, high->isEnabled(ls) ? 1 : 0,
			high->getViewportMask(ls), high->getMinVisibleDistance(ls), high->getMaxVisibleDistance(ls),
			high->getMaterial(ls) ? high->getMaterial(ls)->getFilePath().get() : "<none>");

		// bake only what is actually visible up close:
		// skip disabled surfaces, surfaces hidden by viewport mask,
		// and LOD shells that are not visible at zero distance
		if (!isSurfaceBakeable(high, ls))
		{
			Log::message("Baker: surface %d is not visible up close (disabled/mask/LOD), skipping\n", s);
			continue;
		}

		MaterialPtr mat = high->getMaterial(ls);
		if (mat)
		{
			if (mat->findParameter("albedo_color") >= 0)
				surf.albedoColor = mat->getParameterFloat4("albedo_color");
			if (mat->findParameter("metalness") >= 0)
				surf.metalness = mat->getParameterFloat("metalness");
			if (mat->findParameter("roughness") >= 0)
				surf.roughness = mat->getParameterFloat("roughness");
			if (mat->findParameter("uv_transform") >= 0)
			{
				surf.uvTransform = mat->getParameterFloat4("uv_transform");
				if (surf.uvTransform.x != 1.0f || surf.uvTransform.y != 1.0f
					|| surf.uvTransform.z != 0.0f || surf.uvTransform.w != 0.0f)
					Log::message("Baker: surface %d uses uv_transform %g %g %g %g\n", s,
						surf.uvTransform.x, surf.uvTransform.y, surf.uvTransform.z, surf.uvTransform.w);
			}
			if (mat->findTexture("albedo") >= 0)
				surf.albedo = loadTexture(mat->getTexturePath("albedo"), textureCache);
			if (mat->findTexture("shading") >= 0)
				surf.shading = loadTexture(mat->getTexturePath("shading"), textureCache);
			if (mat->findTexture("normal") >= 0)
			{
				surf.normal = loadTexture(mat->getTexturePath("normal"), textureCache);
				if (surf.normal)
				{
					surf.normalInvertG = sourceNormalInvertG(mat->getTexturePath("normal"));
					if (surf.normalInvertG)
						Log::message("Baker: surface %d normal map is imported with invert_g, compensating\n", s);
				}
			}
			if (settings.bakeEmission && mat->findState("emission") >= 0 && mat->getState("emission"))
			{
				surf.emissionOn = true;
				if (mat->findParameter("emission_color") >= 0)
					surf.emissionColor = mat->getParameterFloat4("emission_color");
				if (mat->findParameter("emission_scale") >= 0)
					surf.emissionScale = mat->getParameterFloat("emission_scale");
				if (mat->findTexture("emission") >= 0)
					surf.emission = loadTexture(mat->getTexturePath("emission"), textureCache);
			}
		}
		anyAlbedo |= (bool)surf.albedo;
		anyShading |= (bool)surf.shading;
		anyNormalMap |= (bool)surf.normal;

		const Vector<int> &cind = highMesh->getCIndices(ls);
		const Vector<int> &tind = highMesh->getTIndices(ls);
		int numTris = cind.size() / 3;
		if (numTris <= 0)
			continue;

		int numT = highMesh->getNumTangents(ls);
		surf.hasUV = highMesh->getNumTexCoords0(ls) > 0;
		surf.uv.resize(surf.hasUV ? highMesh->getNumTexCoords0(ls) : 0);
		for (int i = 0; i < int(surf.uv.size()); i++)
			surf.uv[i] = highMesh->getTexCoord0(i, ls);
		surf.hasUV1 = surf.hasUV && highMesh->getNumTexCoords1(ls) >= int(surf.uv.size());
		surf.uv1.resize(surf.hasUV1 ? surf.uv.size() : 0);
		for (int i = 0; i < int(surf.uv1.size()); i++)
			surf.uv1[i] = highMesh->getTexCoord1(i, ls);
		surf.n.resize(numT);
		surf.t.resize(numT);
		surf.sw.resize(numT);
		for (int i = 0; i < numT; i++)
		{
			quat q = highMesh->getTangent(i, ls);
			surf.n[i] = normalize(highNm * q.getNormal());
			surf.t[i] = normalize(highNm * q.getTangent());
			surf.sw[i] = (q.w < 0.0f) ? -1.0f : 1.0f;
		}

		// GPU "as rendered" mode: use captures prepared by the UI (readbacks
		// complete only between editor frames, so they cannot be made here)
		if (settings.gpuMode && surf.hasUV && settings.gpuCaptures
			&& s < int(settings.gpuCaptures->size()) && (*settings.gpuCaptures)[s].valid())
		{
			surf.gpu = (*settings.gpuCaptures)[s];
			if (!surf.gpu.atlasUV.empty())
			{
				// chart-repacked capture: per-corner atlas coordinates
				if (int(surf.gpu.atlasUV.size()) == int(tind.size()))
					surf.useGpu = true;
				else
					Log::warning("Baker: surface %d capture atlas size mismatch (%d vs %d), "
						"falling back to texture sampling\n",
						s, int(surf.gpu.atlasUV.size()), int(tind.size()));
			}
			// legacy path: a UV1-space capture needs the surface's UV1 to sample it back
			else if (surf.gpu.uvChannel == 1 && !surf.hasUV1)
				Log::warning("Baker: surface %d capture is in UV1 but the mesh copy has no UV1, "
					"falling back to texture sampling\n", s);
			else
				surf.useGpu = true;
		}
		else if (settings.gpuMode)
		{
			Log::warning("Baker: no GPU capture for surface %d, falling back to texture sampling\n", s);
		}

		srcTris.reserve(srcTris.size() + numTris);
		for (int k = 0; k < numTris; k++)
		{
			SourceTri tr;
			vec3 p0 = highTm * highMesh->getVertex(cind[k * 3 + 0], ls);
			vec3 p1 = highTm * highMesh->getVertex(cind[k * 3 + 1], ls);
			vec3 p2 = highTm * highMesh->getVertex(cind[k * 3 + 2], ls);
			tr.p0 = p0;
			tr.e1 = p1 - p0;
			tr.e2 = p2 - p0;
			tr.surface = s;
			tr.t0 = tind[k * 3 + 0];
			tr.t1 = tind[k * 3 + 1];
			tr.t2 = tind[k * 3 + 2];
			tr.corner = k * 3;
			srcTris.push_back(tr);
		}
	}
	} // objects
	} // groups

	for (size_t gi = 0; gi < groups.size(); gi++)
		if (groupTris[gi].empty())
			return fail("A bake group's high-poly has no triangles.");

	//--------------------------------------------------------------------------
	// Extract low-poly geometry (all groups; the parts share one UV layout).
	//--------------------------------------------------------------------------
	std::vector<TargetTri> tgtTris;
	for (size_t gi = 0; gi < groups.size(); gi++)
	for (const Ptr<ObjectMeshStatic> &low : groups[gi].lows)
	{
		Ptr<ConstMesh> lowMesh = low->getMeshForceRAM();
		if (!lowMesh)
			return fail("Cannot get the low-poly model geometry.");
		mat4 lowTm = mat4(low->getWorldTransform());
		mat3 lowNm = transpose(inverse(mat3(lowTm)));

		for (int s = 0; s < lowMesh->getNumSurfaces(); s++)
		{
			if (lowMesh->getNumTexCoords0(s) <= 0)
				return fail("The low-poly model has no UV map (UV0).");

			const Vector<int> &cind = lowMesh->getCIndices(s);
			const Vector<int> &tind = lowMesh->getTIndices(s);
			int numTris = cind.size() / 3;

			tgtTris.reserve(tgtTris.size() + numTris);
			for (int k = 0; k < numTris; k++)
			{
				TargetTri tr;
				tr.p0 = lowTm * lowMesh->getVertex(cind[k * 3 + 0], s);
				tr.p1 = lowTm * lowMesh->getVertex(cind[k * 3 + 1], s);
				tr.p2 = lowTm * lowMesh->getVertex(cind[k * 3 + 2], s);
				int i0 = tind[k * 3 + 0], i1 = tind[k * 3 + 1], i2 = tind[k * 3 + 2];
				quat q0 = lowMesh->getTangent(i0, s);
				quat q1 = lowMesh->getTangent(i1, s);
				quat q2 = lowMesh->getTangent(i2, s);
				tr.n0 = normalize(lowNm * q0.getNormal());
				tr.n1 = normalize(lowNm * q1.getNormal());
				tr.n2 = normalize(lowNm * q2.getNormal());
				tr.t0 = normalize(lowNm * q0.getTangent());
				tr.t1 = normalize(lowNm * q1.getTangent());
				tr.t2 = normalize(lowNm * q2.getTangent());
				tr.signW = (q0.w < 0.0f) ? -1.0f : 1.0f;
				tr.uv0 = lowMesh->getTexCoord0(i0, s);
				tr.uv1 = lowMesh->getTexCoord0(i1, s);
				tr.uv2 = lowMesh->getTexCoord0(i2, s);
				tr.group = int(gi);
				tgtTris.push_back(tr);
			}
		}
	}

	if (tgtTris.empty())
		return fail("The low-poly model has no triangles.");

	// Smoothed cage normals: average area-weighted face normals over vertices
	// merged by position, so rays don't split at hard edges and UV seams.
	{
		std::map<QuantizedPos, vec3> smoothed;
		auto add = [&smoothed](const vec3 &p, const vec3 &n) {
			auto res = smoothed.insert({quantize(p), n});
			if (!res.second)
				res.first->second += n;
		};
		for (const TargetTri &tr : tgtTris)
		{
			vec3 faceN = cross(tr.p1 - tr.p0, tr.p2 - tr.p0); // area-weighted
			add(tr.p0, faceN);
			add(tr.p1, faceN);
			add(tr.p2, faceN);
		}
		for (TargetTri &tr : tgtTris)
		{
			auto pick = [&smoothed](const vec3 &p, const vec3 &fallback) {
				auto it = smoothed.find(quantize(p));
				if (it == smoothed.end() || length2(it->second) < 1e-20f)
					return fallback;
				return normalize(it->second);
			};
			tr.r0 = pick(tr.p0, tr.n0);
			tr.r1 = pick(tr.p1, tr.n1);
			tr.r2 = pick(tr.p2, tr.n2);
			vec3 fn = cross(tr.p1 - tr.p0, tr.p2 - tr.p0);
			tr.faceN = length2(fn) > 1e-20f ? normalize(fn) : tr.n0;
		}
	}

	//--------------------------------------------------------------------------
	// Skew mask: painted on the low-poly into its surface custom texture slot
	// (white = rays along the geometric normal, black = smoothed cage normal).
	// One mask in the shared UV layout — the first assigned one found is used.
	//--------------------------------------------------------------------------
	ImagePtr skewMask;
	if (settings.useSkewMask)
	{
		for (const BakeGroup &g : groups)
		{
			for (const Ptr<ObjectMeshStatic> &low : g.lows)
			{
				for (int s = 0; s < low->getNumSurfaces() && !skewMask; s++)
				{
					if (!low->isSurfaceCustomTextureEnabled(s))
						continue;
					skewMask = loadTexture(low->getSurfaceCustomTexturePath(s), textureCache);
					if (skewMask)
					{
						// coverage stats: catches "painted but not saved" and orientation issues
						long long painted = 0;
						const int mw = skewMask->getWidth(), mh = skewMask->getHeight();
						for (int my = 0; my < mh; my++)
							for (int mx = 0; mx < mw; mx++)
								if (skewMask->toVec4(skewMask->get2D(ivec2(mx, my))).x > 0.1f)
									painted++;
						Log::message("Baker: skew mask \"%s\" (%dx%d, painted %.1f%%)\n",
							resolveAssetPath(low->getSurfaceCustomTexturePath(s)).get(),
							mw, mh, 100.0 * double(painted) / double((long long)mw * mh));
						if (painted == 0)
						{
							Log::warning("Baker: skew mask is fully black — ignored\n");
							skewMask = ImagePtr();
						}
					}
				}
				if (skewMask)
					break;
			}
			if (skewMask)
				break;
		}
	}

	if (!progress(10, "Building BVH..."))
	{
		result.cancelled = true;
		return result;
	}

	std::vector<BVH> bvhs(groups.size());
	for (size_t gi = 0; gi < groups.size(); gi++)
		bvhs[gi].build(groupTris[gi]);

	//--------------------------------------------------------------------------
	// Rasterize + trace (worker threads own disjoint row bands).
	//--------------------------------------------------------------------------
	const int res = settings.resolution;
	const float frontal = Math::max(settings.frontalDistance, 1e-5f);
	const float rear = Math::max(settings.rearDistance, 0.0f);
	const float rayLength = frontal + rear;
	const size_t pixelCount = size_t(res) * size_t(res);

	// accumulators: albedo rgba, shading rgba, normal xyz (target tangent space), weight
	std::vector<float> accAlbedo(pixelCount * 4, 0.0f);
	std::vector<float> accShading(pixelCount * 4, 0.0f);
	std::vector<float> accNormal(pixelCount * 3, 0.0f);
	std::vector<float> accEmission(settings.bakeEmission ? pixelCount * 4 : 0, 0.0f);
	std::vector<float> accWeight(pixelCount, 0.0f);

	// subsample offsets
	std::vector<vec2> offsets;
	int grid = 1;
	if (settings.supersamples >= 64)
		grid = 8;
	else if (settings.supersamples >= 16)
		grid = 4;
	else if (settings.supersamples >= 4)
		grid = 2;
	for (int sy = 0; sy < grid; sy++)
		for (int sx = 0; sx < grid; sx++)
			offsets.push_back(vec2((sx + 0.5f) / grid, (sy + 0.5f) / grid));

	int numWorkers = int(std::thread::hardware_concurrency());
	if (numWorkers < 1)
		numWorkers = 4;
	numWorkers = Math::min(numWorkers, res);

	std::atomic<long long> trisDone(0);
	std::atomic<bool> cancelFlag(false);
	const long long trisTotal = (long long)tgtTris.size() * numWorkers;

	// hit statistics (for diagnostics)
	std::atomic<long long> statFront(0), statRear(0), statBackface(0), statMiss(0);

	auto worker = [&](int bandY0, int bandY1) {
		long long locFront = 0, locRear = 0, locBackface = 0, locMiss = 0;
		for (const TargetTri &tr : tgtTris)
		{
			if (cancelFlag.load(std::memory_order_relaxed))
				return;
			trisDone.fetch_add(1, std::memory_order_relaxed);

			// rays of this triangle see only its own bake group's high-poly
			const std::vector<SourceTri> &srcTris = groupTris[tr.group];
			const BVH &bvh = bvhs[tr.group];

			// UV triangle in pixel space
			vec2 a = tr.uv0 * float(res);
			vec2 b = tr.uv1 * float(res);
			vec2 c = tr.uv2 * float(res);
			float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
			if (Math::abs(area) < 1e-9f)
				continue;
			float invArea = 1.0f / area;

			int minX = Math::max(int(Math::floor(Math::min(a.x, Math::min(b.x, c.x)))), 0);
			int maxX = Math::min(int(Math::ceil(Math::max(a.x, Math::max(b.x, c.x)))), res - 1);
			int minY = Math::max(int(Math::floor(Math::min(a.y, Math::min(b.y, c.y)))), bandY0);
			int maxY = Math::min(int(Math::ceil(Math::max(a.y, Math::max(b.y, c.y)))), bandY1 - 1);
			if (minX > maxX || minY > maxY)
				continue;

			for (int y = minY; y <= maxY; y++)
			{
				for (int x = minX; x <= maxX; x++)
				{
					size_t pix = size_t(y) * res + x;
					for (const vec2 &off : offsets)
					{
						vec2 p(x + off.x, y + off.y);
						// barycentrics in UV space
						float l1 = ((p.x - a.x) * (c.y - a.y) - (p.y - a.y) * (c.x - a.x)) * invArea;
						float l2 = ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) * invArea;
						float l0 = 1.0f - l1 - l2;
						const float eps = -1e-4f;
						if (l0 < eps || l1 < eps || l2 < eps)
							continue;

						vec3 pos = tr.p0 * l0 + tr.p1 * l1 + tr.p2 * l2;
						// interpolate normal/tangent linearly and rebuild the basis exactly
						// like the engine fragment shader (normalizationTBN)
						TBN lowTBN = buildTBN(tr.n0 * l0 + tr.n1 * l1 + tr.n2 * l2,
							tr.t0 * l0 + tr.t1 * l1 + tr.t2 * l2, tr.signW);
						const vec3 &nrm = lowTBN.n;
						const vec3 &tan = lowTBN.t;
						const vec3 &bin = lowTBN.b;
						vec3 rayN = settings.raysAlongShading
							? nrm
							: normalize(tr.r0 * l0 + tr.r1 * l1 + tr.r2 * l2);
						float skew = 0.0f;
						if (skewMask)
						{
							// painted skew mask lives in the same UV space as the output
							skew = skewMask->toVec4(
								skewMask->get2D(vec2(p.x / float(res), p.y / float(res)))).x;
							if (skew > 0.001f)
								rayN = normalize(rayN * (1.0f - skew) + tr.faceN * skew);
						}

						// trace from the cage surface inward along the smoothed normal and
						// take the FIRST front-facing hit (the outermost high-poly detail),
						// like conventional bakers do. Cage distance therefore controls how
						// far above the low-poly surface details are still captured.
						vec3 orig = pos + rayN * frontal;
						vec3 dir = -rayN;
						auto scorer = [&](int tri, float t, float u, float v) -> float {
							const SourceTri &str = srcTris[tri];
							const SourceSurface &ssurf = srcSurfaces[str.surface];
							float sw0 = 1.0f - u - v;
							vec3 sn = ssurf.n[str.t0] * sw0 + ssurf.n[str.t1] * u + ssurf.n[str.t2] * v;
							float score = t; // smaller t = closer to the cage = more outward
							if (dot(sn, dir) > 0.0f)
								score += rayLength * 100.0f; // back-facing: use only if nothing better
							return score;
						};
						float hitT, hitU, hitV;
						int hitTri = bvh.trace(srcTris, orig, dir, rayLength, scorer, hitT, hitU, hitV);
						if (hitTri < 0)
						{
							locMiss++;
							continue;
						}

						const SourceTri &st = srcTris[hitTri];
						const SourceSurface &surf = srcSurfaces[st.surface];
						float w0 = 1.0f - hitU - hitV;

						TBN srcTBN = buildTBN(
							surf.n[st.t0] * w0 + surf.n[st.t1] * hitU + surf.n[st.t2] * hitV,
							surf.t[st.t0] * w0 + surf.t[st.t1] * hitU + surf.t[st.t2] * hitV,
							surf.sw[st.t0]);
						const vec3 &srcN = srcTBN.n;

						vec4 debugColor(0.0f, 0.0f, 0.0f, 1.0f);
						{
							vec3 hn = srcN;
							if (dot(hn, dir) > 0.0f)
							{
								locBackface++;
								debugColor = vec4(1.0f, 0.0f, 0.0f, 1.0f); // red
							}
							else if (hitT <= frontal)
							{
								locFront++;
								debugColor = vec4(0.0f, 1.0f, 0.0f, 1.0f); // green
							}
							else
							{
								locRear++;
								debugColor = vec4(0.0f, 0.0f, 1.0f, 1.0f); // blue
							}
						}

						// fallbacks when the material has no textures: engine constants
						vec4 albedo = surf.albedoColor;
						vec4 shading(surf.metalness, surf.roughness, 0.5f, 0.0f);
						vec4 emission(0.0f, 0.0f, 0.0f, 1.0f);
						if (surf.emissionOn)
							emission = vec4(surf.emissionColor.xyz * surf.emissionScale, 1.0f);
						if (surf.hasUV)
						{
							vec2 suvRaw = surf.uv[st.t0] * w0 + surf.uv[st.t1] * hitU + surf.uv[st.t2] * hitV;
							vec3 worldN = srcN;

							// sample the "as rendered" unwrap gbuffer captures through the
							// chart-repacked atlas (unique by construction); the pack
							// transform is affine, so barycentric interpolation of the
							// per-tvertex atlas coords is exact. Triangles of charts too
							// small to capture are marked with negative coords — those
							// take the CPU texture path below.
							bool gpuHit = surf.useGpu;
							vec2 cuv(0.0f, 0.0f);
							if (gpuHit)
							{
								if (!surf.gpu.atlasUV.empty())
								{
									// per-corner atlas coords: entries [corner .. corner+2]
									cuv = surf.gpu.atlasUV[st.corner] * w0
										+ surf.gpu.atlasUV[st.corner + 1] * hitU
										+ surf.gpu.atlasUV[st.corner + 2] * hitV;
									if (cuv.x < 0.0f || cuv.y < 0.0f)
										gpuHit = false; // uncapturable chart
								}
								else
								{
									// legacy path: capture rendered in a raw UV channel
									vec2 gpuUV = suvRaw;
									if (surf.gpu.uvChannel == 1)
										gpuUV = surf.uv1[st.t0] * w0 + surf.uv1[st.t1] * hitU + surf.uv1[st.t2] * hitV;
									cuv = vec2((gpuUV.x - surf.gpu.uvMin.x) / surf.gpu.uvScale.x,
										(gpuUV.y - surf.gpu.uvMin.y) / surf.gpu.uvScale.y);
								}
							}

							if (gpuHit)
							{
								cuv.x = saturate(cuv.x);
								cuv.y = saturate(cuv.y);

								vec4 a4 = surf.gpu.albedo->toVec4(surf.gpu.albedo->get2D(cuv));
								vec4 s4 = surf.gpu.shading->toVec4(surf.gpu.shading->get2D(cuv));
								// octahedral-packed normal: bilinear filtering would break the
								// bit packing, sample nearest texel
								int gw = surf.gpu.normal->getWidth();
								int gh = surf.gpu.normal->getHeight();
								ivec2 ncoord(Math::clamp(int(cuv.x * gw), 0, gw - 1),
									Math::clamp(int(cuv.y * gh), 0, gh - 1));
								vec4 n4 = surf.gpu.normal->toVec4(surf.gpu.normal->get2D(ncoord));

								albedo = a4; // the gbuffer render target already stores sRGB-encoded albedo
								// _sh layout: R=metalness, G=roughness, B=specular(f0), A=microfiber
								shading = vec4(s4.x, n4.w, s4.y, s4.w);
								if (surf.gpu.emission)
								{
									vec4 e4 = surf.gpu.emission->toVec4(surf.gpu.emission->get2D(cuv));
									emission = vec4(e4.xyz, 1.0f);
								}

								vec3 ts = BakeGpu::unpackGBufferNormal(n4);
								worldN = normalize(srcTBN.t * ts.x + srcTBN.b * ts.y + srcTBN.n * ts.z);
							}
							else
							{
								vec2 suv = suvRaw;
								// apply the material's base UV tiling, like the engine shader does,
								// and wrap into [0..1) — get2D behavior outside is undocumented
								suv = vec2(suv.x * surf.uvTransform.x + surf.uvTransform.z,
									suv.y * surf.uvTransform.y + surf.uvTransform.w);
								suv.x -= Math::floor(suv.x);
								suv.y -= Math::floor(suv.y);
								if (surf.albedo)
								{
									// the engine multiplies the albedo texture by albedo_color
									albedo = surf.albedo->toVec4(surf.albedo->get2D(suv)) * surf.albedoColor;
								}
								if (surf.shading)
									shading = surf.shading->toVec4(surf.shading->get2D(suv));
								if (surf.emissionOn && surf.emission)
								{
									vec4 e4 = surf.emission->toVec4(surf.emission->get2D(suv));
									emission = vec4(e4.xyz * surf.emissionColor.xyz * surf.emissionScale, 1.0f);
								}

								if (surf.normal)
								{
									const vec3 &stn = srcTBN.t;
									const vec3 &sbn = srcTBN.b;
									vec4 nm = surf.normal->toVec4(surf.normal->get2D(suv));
									float nx = nm.x * 2.0f - 1.0f;
									float ny = nm.y * 2.0f - 1.0f;
									if (settings.flipNormalY)
										ny = -ny;
									if (surf.normalInvertG)
										ny = -ny; // renderer sees the imported (G-inverted) copy
									float nz = Math::sqrt(Math::max(1.0f - nx * nx - ny * ny, 0.0f));
									worldN = normalize(stn * nx + sbn * ny + srcN * nz);
								}
							}

							// into target tangent space
							vec3 tsn(dot(tan, worldN), dot(bin, worldN), dot(nrm, worldN));
							accNormal[pix * 3 + 0] += tsn.x;
							accNormal[pix * 3 + 1] += tsn.y;
							accNormal[pix * 3 + 2] += tsn.z;
						}
						else
						{
							// no UV on high-poly: bake geometric normal only
							accNormal[pix * 3 + 0] += dot(tan, srcN);
							accNormal[pix * 3 + 1] += dot(bin, srcN);
							accNormal[pix * 3 + 2] += dot(nrm, srcN);
						}

						if (settings.debugZones)
						{
							albedo = debugColor;
							// overlay the skew mask in yellow so its coverage is visible
							if (skew > 0.001f)
								albedo = albedo * (1.0f - skew * 0.7f)
									+ vec4(1.0f, 1.0f, 0.0f, 1.0f) * (skew * 0.7f);
						}
						accAlbedo[pix * 4 + 0] += albedo.x;
						accAlbedo[pix * 4 + 1] += albedo.y;
						accAlbedo[pix * 4 + 2] += albedo.z;
						accAlbedo[pix * 4 + 3] += albedo.w;
						accShading[pix * 4 + 0] += shading.x;
						accShading[pix * 4 + 1] += shading.y;
						accShading[pix * 4 + 2] += shading.z;
						accShading[pix * 4 + 3] += shading.w;
						if (settings.bakeEmission)
						{
							accEmission[pix * 4 + 0] += emission.x;
							accEmission[pix * 4 + 1] += emission.y;
							accEmission[pix * 4 + 2] += emission.z;
							accEmission[pix * 4 + 3] += emission.w;
						}
						accWeight[pix] += 1.0f;
					}
				}
			}
		}
		statFront += locFront;
		statRear += locRear;
		statBackface += locBackface;
		statMiss += locMiss;
	};

	{
		std::vector<std::thread> threads;
		int rowsPerBand = (res + numWorkers - 1) / numWorkers;
		for (int i = 0; i < numWorkers; i++)
		{
			int y0 = i * rowsPerBand;
			int y1 = Math::min(y0 + rowsPerBand, res);
			if (y0 >= y1)
				break;
			threads.emplace_back(worker, y0, y1);
		}

		bool keepGoing = true;
		while (trisDone.load() < trisTotal && !cancelFlag.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			int percent = 15 + int(70.0 * double(trisDone.load()) / double(trisTotal > 0 ? trisTotal : 1));
			keepGoing = progress(Math::min(percent, 85), "Baking...");
			if (!keepGoing)
				cancelFlag.store(true);
		}
		for (auto &th : threads)
			th.join();

		if (cancelFlag.load())
		{
			result.cancelled = true;
			return result;
		}
	}

	Log::message("Baker: ray stats: front(above surface)=%lld rear(below surface)=%lld backface=%lld miss=%lld\n",
		statFront.load(), statRear.load(), statBackface.load(), statMiss.load());

	if (!progress(86, "Processing edges..."))
	{
		result.cancelled = true;
		return result;
	}

	//--------------------------------------------------------------------------
	// Resolve + dilation.
	//--------------------------------------------------------------------------
	std::vector<unsigned char> covered(pixelCount, 0);
	for (size_t i = 0; i < pixelCount; i++)
	{
		float w = accWeight[i];
		if (w <= 0.0f)
			continue;
		covered[i] = 1;
		float inv = 1.0f / w;
		for (int c = 0; c < 4; c++)
		{
			accAlbedo[i * 4 + c] *= inv;
			accShading[i * 4 + c] *= inv;
			if (settings.bakeEmission)
				accEmission[i * 4 + c] *= inv;
		}
		vec3 n(accNormal[i * 3 + 0], accNormal[i * 3 + 1], accNormal[i * 3 + 2]);
		n = normalize(n);
		accNormal[i * 3 + 0] = n.x;
		accNormal[i * 3 + 1] = n.y;
		accNormal[i * 3 + 2] = n.z;
	}

	// wavefront dilation
	{
		std::vector<int> frontier;
		std::vector<int> next;
		auto isCovered = [&](int x, int y) -> bool {
			return x >= 0 && y >= 0 && x < res && y < res && covered[size_t(y) * res + x] != 0;
		};
		for (int pass = 0; pass < settings.dilationPixels; pass++)
		{
			next.clear();
			for (int y = 0; y < res; y++)
				for (int x = 0; x < res; x++)
				{
					size_t i = size_t(y) * res + x;
					if (covered[i])
						continue;
					// average covered neighbors
					float alb[4] = {0, 0, 0, 0}, sh[4] = {0, 0, 0, 0}, em[4] = {0, 0, 0, 0}, nr[3] = {0, 0, 0};
					int cnt = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++)
						{
							if (!dx && !dy)
								continue;
							if (!isCovered(x + dx, y + dy))
								continue;
							size_t j = size_t(y + dy) * res + (x + dx);
							for (int cch = 0; cch < 4; cch++)
							{
								alb[cch] += accAlbedo[j * 4 + cch];
								sh[cch] += accShading[j * 4 + cch];
								if (settings.bakeEmission)
									em[cch] += accEmission[j * 4 + cch];
							}
							for (int cch = 0; cch < 3; cch++)
								nr[cch] += accNormal[j * 3 + cch];
							cnt++;
						}
					if (!cnt)
						continue;
					float inv = 1.0f / cnt;
					for (int cch = 0; cch < 4; cch++)
					{
						accAlbedo[i * 4 + cch] = alb[cch] * inv;
						accShading[i * 4 + cch] = sh[cch] * inv;
						if (settings.bakeEmission)
							accEmission[i * 4 + cch] = em[cch] * inv;
					}
					vec3 n(nr[0] * inv, nr[1] * inv, nr[2] * inv);
					n = normalize(n);
					accNormal[i * 3 + 0] = n.x;
					accNormal[i * 3 + 1] = n.y;
					accNormal[i * 3 + 2] = n.z;
					next.push_back(int(i));
				}
			if (next.empty())
				break;
			for (int i : next)
				covered[i] = 1;
		}
	}

	if (!progress(92, "Saving textures..."))
	{
		result.cancelled = true;
		return result;
	}

	//--------------------------------------------------------------------------
	// Output paths.
	//--------------------------------------------------------------------------
	String highAssetPath = resolveAssetPath(firstHigh->getMeshPath());
	String lowAssetPath = resolveAssetPath(firstLow->getMeshPath());

	// user-provided output name wins; sanitize filesystem-reserved characters
	String baseName;
	{
		std::string clean;
		for (int i = 0; i < settings.outputName.size(); i++)
		{
			const char c = settings.outputName[i];
			clean += (c && strchr("\\/:*?\"<>|", c)) ? '_' : c;
		}
		baseName = String(clean.c_str()).trim();
	}
	if (baseName.empty())
		baseName = suggestBaseName(firstHigh);

	String anchorPath = lowAssetPath;
	if (anchorPath.empty() || isGuidPath(anchorPath))
		anchorPath = highAssetPath;
	if (anchorPath.empty())
		return fail("Neither model has a mesh file — save the models as assets.");
	if (isGuidPath(anchorPath))
		return fail("Cannot determine the models' asset folder (paths are GUID references).");

	String virtualDir = pathDir(anchorPath);
	// Build the on-disk path from the data root: FileSystem::getAbsolutePath resolves
	// asset paths into .runtimes, which is not where new asset files belong.
	String dataPath = Engine::get()->getDataPath();
	if (dataPath.empty())
		return fail("Cannot determine the project data folder.");
	if (dataPath[dataPath.size() - 1] != '/' && dataPath[dataPath.size() - 1] != '\\')
		dataPath += "/";
	String absDir = dataPath + virtualDir;
	Log::message("Baker: data path \"%s\"\n", dataPath.get());
	Log::message("Baker: output dir \"%s\"\n", absDir.get());

	//--------------------------------------------------------------------------
	// Write images.
	//--------------------------------------------------------------------------
	auto writeImage = [&](const char *postfix, int channels, const std::vector<float> &data,
						  int stride, String &outVirtual, bool sixteenBit = false) -> bool {
		ImagePtr img = Image::create();
		int format = channels == 3 ? (sixteenBit ? Image::FORMAT_RGB16 : Image::FORMAT_RGB8)
								   : Image::FORMAT_RGBA8;
		img->create2D(res, res, format);
		for (int y = 0; y < res; y++)
			for (int x = 0; x < res; x++)
			{
				size_t i = size_t(y) * res + x;
				vec4 color(0.0f, 0.0f, 0.0f, 1.0f);
				for (int c = 0; c < Math::min(stride, 4); c++)
					color[c] = data[i * stride + c];
				img->set2D(x, y, img->toPixel(color));
			}
		String fileName = baseName + postfix + ".png";
		String absPath = absDir + fileName;

		// Image::save() on a path of an already imported asset is intercepted by the
		// engine virtual file system and written into the .runtimes copy instead of
		// the source file. Save to an OS temp file and copy over via the OS instead.
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path tmpPath = fs::temp_directory_path(ec) / (String(baseName + postfix).get() + std::string("_baker_tmp.png"));
		if (ec)
		{
			Log::error("Baker: temp dir unavailable: %s\n", ec.message().c_str());
			return false;
		}
		if (!img->save(tmpPath.string().c_str()))
		{
			Log::error("Baker: failed to save \"%s\"\n", tmpPath.string().c_str());
			return false;
		}
		fs::copy_file(tmpPath, fs::u8path(absPath.get()), fs::copy_options::overwrite_existing, ec);
		std::error_code ec2;
		fs::remove(tmpPath, ec2);
		if (ec)
		{
			Log::error("Baker: failed to copy texture to \"%s\": %s\n", absPath.get(), ec.message().c_str());
			return false;
		}
		Log::message("Baker: saved \"%s\"\n", absPath.get());
		outVirtual = virtualDir + fileName;
		return true;
	};

	if (anyAlbedo || settings.debugZones)
	{
		if (!writeImage("_alb", 4, accAlbedo, 4, result.albedoPath))
			return fail("Failed to save the albedo texture.");
	}
	if (anyShading)
	{
		if (!writeImage("_sh", 4, accShading, 4, result.shadingPath))
			return fail("Failed to save the shading texture (_sh).");
	}
	if (settings.bakeEmission)
	{
		if (!writeImage("_e", 4, accEmission, 4, result.emissionPath))
			return fail("Failed to save the emission texture (_e).");
	}

	// normal: encode from [-1..1] to [0..1]
	{
		std::vector<float> encoded(pixelCount * 3);
		for (size_t i = 0; i < pixelCount; i++)
		{
			// default: flat normal for uncovered pixels
			vec3 n(0.0f, 0.0f, 1.0f);
			if (covered[i])
				n = vec3(accNormal[i * 3 + 0], accNormal[i * 3 + 1], accNormal[i * 3 + 2]);
			if (settings.flipNormalY)
				n.y = -n.y;
			encoded[i * 3 + 0] = n.x * 0.5f + 0.5f;
			encoded[i * 3 + 1] = n.y * 0.5f + 0.5f;
			encoded[i * 3 + 2] = n.z * 0.5f + 0.5f;
		}
		if (!writeImage("_n", 3, encoded, 3, result.normalPath, true))
			return fail("Failed to save the normal map.");
	}

	progress(100, "Done");
	result.success = true;
	Log::message("Baker: done. albedo=\"%s\" shading=\"%s\" normal=\"%s\" emission=\"%s\"\n",
		result.albedoPath.get(), result.shadingPath.get(), result.normalPath.get(),
		result.emissionPath.get());
	return result;
}

// Runs AFTER the baked textures are imported as assets: assigning a texture
// path that is not an asset yet binds the raw file, bypassing the import
// pipeline (a raw normal map renders broken until the next reimport).
void assignMaterial(const std::vector<BakeGroup> &groups, Result &result)
{
	if (groups.empty() || groups[0].lows.empty() || result.normalPath.empty())
		return;
	const Ptr<ObjectMeshStatic> &firstLow = groups[0].lows[0];

	String virtualDir = pathDir(result.normalPath);
	String baseName = pathBaseName(result.normalPath);
	if (baseName.size() > 2 && !strcmp(baseName.get() + baseName.size() - 2, "_n"))
		baseName = baseName.substr(0, baseName.size() - 2);

	MaterialPtr mat = firstLow->getMaterial(0);
	if (!mat || mat->isBase())
	{
		MaterialPtr base;
		if (mat && mat->isBase())
			base = mat;
		if (!base)
			base = Materials::findManualMaterial("mesh_base");
		if (!base)
			base = Materials::findManualMaterial("Unigine::mesh_base");
		if (!base)
		{
			Log::error("Baker: the mesh_base base material was not found\n");
			return;
		}

		mat = base->inherit();
		String matPath = virtualDir + baseName + "_baked.mat";
		if (!mat->createMaterialFile(matPath.get()))
			Log::warning("Baker: failed to create material file \"%s\"\n", matPath.get());
		for (const BakeGroup &g : groups)
			for (const Ptr<ObjectMeshStatic> &low : g.lows)
				low->setMaterial(mat, "*");
		result.materialPath = matPath;
	}
	else
	{
		// the first part already has an inherited material: bake into it and
		// share it with the other parts
		for (const BakeGroup &g : groups)
			for (const Ptr<ObjectMeshStatic> &low : g.lows)
				if (low->getID() != firstLow->getID())
					low->setMaterial(mat, "*");
		result.materialPath = mat->getFilePath();
	}

	if (!result.albedoPath.empty() && mat->findTexture("albedo") >= 0)
	{
		mat->setTexturePath("albedo", result.albedoPath.get());
		// the baked texture already contains the final color: the material
		// must not tint it again
		if (mat->findParameter("albedo_color") >= 0)
			mat->setParameterFloat4("albedo_color", vec4(1.0f, 1.0f, 1.0f, 1.0f));
	}
	if (!result.shadingPath.empty() && mat->findTexture("shading") >= 0)
	{
		mat->setTexturePath("shading", result.shadingPath.get());
		// the baked _sh stores the final values; the material multipliers must
		// pass them through (mesh_base defaults metalness to 0, killing the R channel)
		if (mat->findParameter("metalness") >= 0)
			mat->setParameterFloat("metalness", 1.0f);
		if (mat->findParameter("roughness") >= 0)
			mat->setParameterFloat("roughness", 1.0f);
	}
	if (!result.normalPath.empty() && mat->findTexture("normal") >= 0)
		mat->setTexturePath("normal", result.normalPath.get());
	if (!result.emissionPath.empty() && mat->findState("emission") >= 0)
	{
		// the baked texture stores the final emission color: neutralize the multipliers
		mat->setState("emission", 1);
		if (mat->findTexture("emission") >= 0)
			mat->setTexturePath("emission", result.emissionPath.get());
		if (mat->findParameter("emission_color") >= 0)
			mat->setParameterFloat4("emission_color", vec4(1.0f, 1.0f, 1.0f, 1.0f));
		if (mat->findParameter("emission_scale") >= 0)
			mat->setParameterFloat("emission_scale", 1.0f);
	}
	mat->save();
	Log::message("Baker: material \"%s\" updated\n", result.materialPath.get());
}

std::vector<Ptr<ObjectMeshStatic>> collectMeshes(const NodePtr &root)
{
	// disabled (hidden) nodes are included on purpose: the UI hides models
	// for convenient mask painting, and hiding must not affect baking
	std::vector<Ptr<ObjectMeshStatic>> result;
	if (!root)
		return result;
	Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(root);
	if (mesh)
		result.push_back(mesh);
	for (int i = 0; i < root->getNumChildren(); i++)
	{
		NodePtr child = root->getChild(i);
		if (!child)
			continue;
		std::vector<Ptr<ObjectMeshStatic>> sub = collectMeshes(child);
		result.insert(result.end(), sub.begin(), sub.end());
	}
	return result;
}

String suggestBaseName(const Ptr<ObjectMeshStatic> &high)
{
	String baseName;
	if (high)
	{
		String assetPath = resolveAssetPath(high->getMeshPath());
		if (!isGuidPath(assetPath))
			baseName = pathBaseName(assetPath);
		if (baseName.empty())
			baseName = high->getName();
	}
	if (baseName.empty())
		baseName = "baked";
	return baseName;
}

bool isSurfaceBakeable(const Ptr<ObjectMeshStatic> &obj, int surface)
{
	if (!obj || surface < 0 || surface >= obj->getNumSurfaces())
		return false;
	if (!obj->isEnabled(surface))
		return false;
	if (obj->getViewportMask(surface) == 0)
		return false;
	if (obj->getMinVisibleDistance(surface) > 0.0f || obj->getMaxVisibleDistance(surface) <= 0.0f)
		return false;
	return true;
}

String createSkewMask(const Ptr<ObjectMeshStatic> &low, int size, String &error)
{
	error = "";
	if (!low)
	{
		error = "The low-poly model is not set.";
		return String();
	}
	String lowAssetPath = resolveAssetPath(low->getMeshPath());
	if (lowAssetPath.empty() || isGuidPath(lowAssetPath))
	{
		error = "The low-poly model has no mesh file — save the model as an asset.";
		return String();
	}
	String dataPath = Engine::get()->getDataPath();
	if (dataPath.empty())
	{
		error = "Cannot determine the project data folder.";
		return String();
	}
	if (dataPath[dataPath.size() - 1] != '/' && dataPath[dataPath.size() - 1] != '\\')
		dataPath += "/";

	String fileName = pathBaseName(lowAssetPath) + "_skew.png";
	String virtualPath = pathDir(lowAssetPath) + fileName;
	String absPath = dataPath + virtualPath;

	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::exists(fs::u8path(absPath.get()), ec))
	{
		ImagePtr img = Image::create();
		img->create2D(size, size, Image::FORMAT_R8);
		unsigned char *px = img->getPixels2D();
		if (px)
			memset(px, 0, img->getPixelsSize());

		// same VFS workaround as the baked textures: save to an OS temp file and
		// copy over, so the write lands in the asset file and not in .runtimes
		fs::path tmpPath = fs::temp_directory_path(ec) / "baker_skew_tmp.png";
		if (ec || !img->save(tmpPath.string().c_str()))
		{
			error = "Failed to save the mask to a temporary file.";
			return String();
		}
		fs::copy_file(tmpPath, fs::u8path(absPath.get()), fs::copy_options::overwrite_existing, ec);
		std::error_code ec2;
		fs::remove(tmpPath, ec2);
		if (ec)
		{
			error = String("Failed to write the mask: ") + absPath;
			return String();
		}
		Log::message("Baker: created skew mask \"%s\" (%dx%d)\n", absPath.get(), size, size);
	}
	else
	{
		Log::message("Baker: skew mask \"%s\" already exists, reusing\n", absPath.get());
	}

	return virtualPath;
}

void assignSkewMask(const Ptr<ObjectMeshStatic> &low, const char *virtualPath)
{
	if (!low || !virtualPath || !*virtualPath)
		return;
	// assign into the slot the editor's Texture Editor paints; must run AFTER
	// the asset is imported, otherwise the engine cannot resolve the path.
	// Use the guid:// form — that is what the editor stores on manual assignment.
	String assignPath = virtualPath;
	UGUID guid = FileSystem::getGUID(virtualPath);
	if (guid.isValid())
		assignPath = String("guid://") + String(guid.makeString().get());
	for (int s = 0; s < low->getNumSurfaces(); s++)
	{
		// enable FIRST: the path setter may be a no-op on a disabled slot
		low->setSurfaceCustomTextureEnabled(true, s);
		low->setSurfaceCustomTextureMode(ObjectMeshStatic::SURFACE_CUSTOM_TEXTURE_MODE_UNIQUE, s);
		low->setSurfaceCustomTexturePath(assignPath.get(), s);
		Log::message("Baker: surface %d custom texture set to \"%s\", read back: enabled=%d path=\"%s\"\n",
			s, assignPath.get(), int(low->isSurfaceCustomTextureEnabled(s)),
			low->getSurfaceCustomTexturePath(s));
	}
}

} // namespace BakeCore
