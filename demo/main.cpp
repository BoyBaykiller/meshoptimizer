#include "../src/meshoptimizer.hpp"

#include "tiny_obj_loader.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

const size_t kCacheSize = 24;

struct Vertex
{
	float px, py, pz;
	float nx, ny, nz;
	float tx, ty;
};

struct Mesh
{
	std::vector<Vertex> vertices;
	std::vector<unsigned int> indices;
};

Mesh generatePlane(unsigned int N)
{
	Mesh result;

	result.vertices.reserve((N + 1) * (N + 1));
	result.indices.reserve(N * N * 6);

	for (unsigned int y = 0; y <= N; ++y)
		for (unsigned int x = 0; x <= N; ++x)
		{
			Vertex v = {float(x), float(y), 0, 0, 0, 1};

			result.vertices.push_back(v);
		}

	for (unsigned int y = 0; y < N; ++y)
		for (unsigned int x = 0; x < N; ++x)
		{
			result.indices.push_back((y + 0) * N + (x + 0));
			result.indices.push_back((y + 0) * N + (x + 1));
			result.indices.push_back((y + 1) * N + (x + 0));

			result.indices.push_back((y + 1) * N + (x + 0));
			result.indices.push_back((y + 0) * N + (x + 1));
			result.indices.push_back((y + 1) * N + (x + 1));
		}

	return result;
}

Mesh readOBJ(const char* path)
{
	using namespace tinyobj;

	attrib_t attrib;
	std::vector<shape_t> shapes;
	std::vector<material_t> materials;
	std::string error;

	if (!LoadObj(&attrib, &shapes, &materials, &error, path))
	{
		printf("Error loading %s: %s\n", path, error.c_str());
		return Mesh();
	}

	size_t total_indices = 0;

	for (auto& s : shapes)
		total_indices += s.mesh.indices.size();

	std::vector<Vertex> vertices;
	vertices.reserve(total_indices);

	for (auto& s : shapes)
		for (auto& i : s.mesh.indices)
		{
			Vertex v =
			    {
			        attrib.vertices[i.vertex_index * 3 + 0],
			        attrib.vertices[i.vertex_index * 3 + 1],
			        attrib.vertices[i.vertex_index * 3 + 2],
			        i.normal_index >= 0 ? attrib.normals[i.normal_index * 3 + 0] : 0,
			        i.normal_index >= 0 ? attrib.normals[i.normal_index * 3 + 1] : 0,
			        i.normal_index >= 0 ? attrib.normals[i.normal_index * 3 + 2] : 0,
			        i.texcoord_index >= 0 ? attrib.texcoords[i.texcoord_index * 2 + 0] : 0,
			        i.texcoord_index >= 0 ? attrib.texcoords[i.texcoord_index * 2 + 1] : 0,
			    };

			vertices.push_back(v);
		}

	Mesh result;

	result.indices.resize(total_indices);

	size_t total_vertices = generateIndexBuffer(&result.indices[0], &vertices[0], total_indices, sizeof(Vertex));

	result.vertices.resize(total_vertices);

	generateVertexBuffer(&result.vertices[0], &result.indices[0], &vertices[0], total_indices, sizeof(Vertex));

	return result;
}

void optNone(Mesh& mesh)
{
}

void optRandomShuffle(Mesh& mesh)
{
	std::vector<unsigned int> faces(mesh.indices.size() / 3);

	for (size_t i = 0; i < faces.size(); ++i)
		faces[i] = static_cast<unsigned int>(i);

	std::random_shuffle(faces.begin(), faces.end());

	std::vector<unsigned int> result(mesh.indices.size());

	for (size_t i = 0; i < faces.size(); ++i)
	{
		result[i * 3 + 0] = mesh.indices[faces[i] * 3 + 0];
		result[i * 3 + 1] = mesh.indices[faces[i] * 3 + 1];
		result[i * 3 + 2] = mesh.indices[faces[i] * 3 + 2];
	}

	mesh.indices.swap(result);
}

void optPostTransform(Mesh& mesh)
{
	std::vector<unsigned int> result(mesh.indices.size());

	optimizePostTransform(&result[0], &mesh.indices[0], mesh.indices.size(), mesh.vertices.size(), kCacheSize);

	mesh.indices.swap(result);
}

void optOverdraw(Mesh& mesh)
{
	std::vector<unsigned int> result(mesh.indices.size());

	std::vector<unsigned int> clusters;
	optimizePostTransform(&result[0], &mesh.indices[0], mesh.indices.size(), mesh.vertices.size(), kCacheSize, &clusters);

	// allow up to 5% worse ACMR to get more reordering opportunities for overdraw
	const float kThreshold = 1.05f;

	optimizeOverdraw(&mesh.indices[0], &result[0], mesh.indices.size(), &mesh.vertices[0].px, sizeof(Vertex), mesh.vertices.size(), clusters, kCacheSize, kThreshold);
}

void optOverdrawOnly(Mesh& mesh)
{
	std::vector<unsigned int> result(mesh.indices.size());

	// use single input cluster encompassing the entire mesh and worst-case ACMR so that overdraw optimizer can sort *all* triangles
	std::vector<unsigned int> clusters(1);
	const float kThreshold = 3.f;

	optimizeOverdraw(&result[0], &mesh.indices[0], mesh.indices.size(), &mesh.vertices[0].px, sizeof(Vertex), mesh.vertices.size(), clusters, kCacheSize, kThreshold);

	mesh.indices.swap(result);
}

void optimize(const Mesh& mesh, const char* name, void (*optf)(Mesh& mesh))
{
	Mesh copy = mesh;

	clock_t start = clock();
	optf(copy);
	clock_t end = clock();

	PostTransformCacheStatistics ptcs = analyzePostTransform(&copy.indices[0], copy.indices.size(), copy.vertices.size(), kCacheSize);
	OverdrawStatistics os = analyzeOverdraw(&copy.indices[0], copy.indices.size(), &copy.vertices[0].px, sizeof(Vertex), copy.vertices.size());

	printf("%-15s: ACMR %f ATVR %f Overdraw %f in %f msec\n", name, ptcs.acmr, ptcs.atvr, os.overdraw, double(end - start) / CLOCKS_PER_SEC * 1000);
}

int main(int argc, char** argv)
{
	Mesh mesh;

	if (argc > 1)
	{
		mesh = readOBJ(argv[1]);

		if (mesh.vertices.empty())
		{
			printf("Mesh %s appears to be empty\n", argv[1]);
			return 0;
		}

		printf("Using %s (%d vertices, %d triangles)\n", argv[1], int(mesh.vertices.size()), int(mesh.indices.size() / 3));
	}
	else
	{
		printf("Usage: %s [.obj file]\n", argv[0]);

		mesh = generatePlane(1000);

		printf("Using a tesselated plane (%d vertices, %d triangles)\n", int(mesh.vertices.size()), int(mesh.indices.size() / 3));
	}

	optimize(mesh, "Original", optNone);
	optimize(mesh, "Random Shuffle", optRandomShuffle);
	optimize(mesh, "Cache", optPostTransform);
	optimize(mesh, "Cache+Overdraw", optOverdraw);
	optimize(mesh, "Overdraw Only", optOverdrawOnly);
}
