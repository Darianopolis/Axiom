#include "axiom_Importer.hpp"

#include <fastgltf/parser.hpp>
#include <fastgltf/glm_element_traits.hpp>

namespace axiom
{
    struct GltfImporter : Importer
    {
        Scene* scene;

        GltfImporter(Scene& scene);

        virtual void Import(std::filesystem::path gltf, std::optional<std::string_view> scene = {});
    };

    nova::Ref<Importer> CreateGltfImporter(Scene& scene)
    {
        return nova::Ref<GltfImporter>::Create(scene);
    }

    GltfImporter::GltfImporter(Scene& _scene)
        : scene(&_scene)
    {}

    struct GltfImporterImpl
    {
        GltfImporter& importer;
        std::unique_ptr<fastgltf::Asset> asset;

        std::vector<nova::Ref<TriMesh>> meshes;

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        u32 debugLongestNodeName;
#endif // ----------------------------------------------------------------------

        void ProcessMeshes();
        void ProcessMesh(const fastgltf::Mesh& mesh);

        void ProcessTextures();

        void ProcessMaterials();

        void ProcessScene(const fastgltf::Scene& scene);
        void ProcessNode(const fastgltf::Node& node, Mat4 parentTransform);
    };

    void GltfImporter::Import(std::filesystem::path gltf, std::optional<std::string_view> sceneName)
    {
        fastgltf::Parser parser{
              fastgltf::Extensions::KHR_texture_transform
            | fastgltf::Extensions::KHR_texture_basisu
            | fastgltf::Extensions::MSFT_texture_dds
            | fastgltf::Extensions::KHR_mesh_quantization
            | fastgltf::Extensions::EXT_meshopt_compression
            | fastgltf::Extensions::KHR_lights_punctual
            | fastgltf::Extensions::EXT_texture_webp
            | fastgltf::Extensions::KHR_materials_specular
            | fastgltf::Extensions::KHR_materials_ior
            | fastgltf::Extensions::KHR_materials_iridescence
            | fastgltf::Extensions::KHR_materials_volume
            | fastgltf::Extensions::KHR_materials_transmission
            | fastgltf::Extensions::KHR_materials_clearcoat
            | fastgltf::Extensions::KHR_materials_emissive_strength
            | fastgltf::Extensions::KHR_materials_sheen
            | fastgltf::Extensions::KHR_materials_unlit
        };

        fastgltf::GltfDataBuffer data;
        data.loadFromFile(gltf);

        auto baseDir = gltf.parent_path();

        constexpr auto GltfOptions =
              fastgltf::Options::DontRequireValidAssetMember
            | fastgltf::Options::AllowDouble
            | fastgltf::Options::LoadGLBBuffers
            | fastgltf::Options::LoadExternalBuffers;

        auto type = fastgltf::determineGltfFileType(&data);

        auto res = type == fastgltf::GltfType::glTF
            ? parser.loadGLTF(&data, baseDir, GltfOptions)
            : parser.loadBinaryGLTF(&data, baseDir, GltfOptions);

        if (!res) {
            NOVA_THROW("Error loading [{}] Message: {}", gltf.string(), fastgltf::getErrorMessage(res.error()));
        }

        GltfImporterImpl impl{ *this };

        impl.asset = std::make_unique<fastgltf::Asset>(std::move(res.get()));

        impl.ProcessMeshes();
        impl.ProcessTextures();
        impl.ProcessMaterials();
        if (sceneName) {
            for (auto& gltfScene : impl.asset->scenes) {
                if (gltfScene.name == *sceneName) {
                    impl.ProcessScene(gltfScene);
                    return;
                }
            }
            NOVA_THROW("Could not find scene: {}", *sceneName);
        } else {
            if (!impl.asset->defaultScene) {
                NOVA_THROW("No scene specified and scene has no default scene!");
            }

            impl.ProcessScene(impl.asset->scenes[impl.asset->defaultScene.value()]);
        }
    }

    void GltfImporterImpl::ProcessMeshes()
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Processing {} meshes...", asset->meshes.size());
#endif // ----------------------------------------------------------------------

        for (auto& mesh : asset->meshes) {

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  - Mesh: {}", mesh.name);
#endif // ----------------------------------------------------------------------

            ProcessMesh(mesh);
        }
    }

    void GltfImporterImpl::ProcessMesh(const fastgltf::Mesh& mesh)
    {
        usz vertexCount = 0;
        usz indexCount = 0;

        std::vector<const fastgltf::Primitive*> primitives;
        primitives.reserve(mesh.primitives.size());

        for (auto& prim : mesh.primitives) {
            if (!prim.indicesAccessor.has_value())
                continue;

            auto positions = prim.findAttribute("POSITION");
            if (positions == prim.attributes.end())
                continue;

            vertexCount += asset->accessors[positions->second].count;
            indexCount  += asset->accessors[prim.indicesAccessor.value()].count;

            primitives.push_back(&prim);
        }

        usz vertexOffset = 0;
        usz indexOffset = 0;

        auto outMesh = nova::Ref<TriMesh>::Create();
        importer.scene->meshes.emplace_back(outMesh);
        meshes.emplace_back(outMesh);
        outMesh->vertices.resize(vertexCount);
        outMesh->indices.resize(indexCount);

        for (auto& prim : primitives) {

            // Indices
            auto& indices = asset->accessors[prim->indicesAccessor.value()];
            fastgltf::iterateAccessorWithIndex<u32>(*asset,
                indices,
                [&](u32 vIndex, usz iIndex) {
                    outMesh->indices[indexOffset + iIndex] = u32(vertexOffset + vIndex);
                });

            // Positions
            auto& positions = asset->accessors[prim->findAttribute("POSITION")->second];
            fastgltf::iterateAccessorWithIndex<Vec3>(*asset,
                positions,
                [&](Vec3 pos, usz index) {
                    outMesh->vertices[vertexOffset + index].position = pos;
                });

            // Normals
            if (auto normals = prim->findAttribute("NORMAL"); normals != prim->attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec3>(*asset,
                    asset->accessors[normals->second], [&](Vec3 normal, usz index) {
                        outMesh->vertices[vertexOffset + index].normal = normal;
                    });
            }

            // Tangents
            if (auto tangents = prim->findAttribute("TANGENT"); tangents != prim->attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec4>(*asset,
                    asset->accessors[tangents->second], [&](Vec4 tangent, usz index) {
                        outMesh->vertices[vertexOffset + index].tangent = tangent;
                    });
            }

            // TexCoords (1)
            if (auto texCoords = prim->findAttribute("TEXCOORD_0"); texCoords != prim->attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec2>(*asset,
                    asset->accessors[texCoords->second], [&](Vec2 texCoord, usz index) {
                        outMesh->vertices[vertexOffset + index].uv = texCoord;
                    });
            }

            vertexOffset += positions.count;
            indexOffset += indices.count;
        }
    }

    void GltfImporterImpl::ProcessTextures()
    {

    }

    void GltfImporterImpl::ProcessMaterials()
    {

    }

    void GltfImporterImpl::ProcessScene(const fastgltf::Scene& scene)
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Processing scene {}...", scene.name);

        debugLongestNodeName = 0;
        for (auto nodeIdx : scene.nodeIndices) {
            [&](this auto&& self, const fastgltf::Node& node) -> void {
                if (node.meshIndex) {
                    debugLongestNodeName = std::max(debugLongestNodeName, u32(node.name.size()));
                }
                for (auto childIdx : node.children) {
                    self(asset->nodes[childIdx]);
                }
            }(asset->nodes[nodeIdx]);
        }
#endif // ----------------------------------------------------------------------

        for (auto nodeIdx : scene.nodeIndices) {
            ProcessNode(asset->nodes[nodeIdx], Mat4(1.f));
        }
    }

    void GltfImporterImpl::ProcessNode(const fastgltf::Node& node, Mat4 parentTransform)
    {

        Mat4 transform = Mat4(1.f);
        if (auto trs = std::get_if<fastgltf::Node::TRS>(&node.transform)) {
            auto translation = std::bit_cast<Vec3>(trs->translation);
            auto rotation = Quat(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]);
            auto scale = std::bit_cast<glm::vec3>(trs->scale);
            transform = glm::translate(Mat4(1.f), translation)
                * glm::mat4_cast(rotation)
                * glm::scale(Mat4(1.f), scale);
        } else if (auto m = std::get_if<fastgltf::Node::TransformMatrix>(&node.transform)) {
            transform = std::bit_cast<Mat4>(*m);
        }

        transform = parentTransform * transform;

        if (node.meshIndex.has_value()) {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  - Mesh Instance: {:>{}} -> {}", node.name, debugLongestNodeName, asset->meshes[node.meshIndex.value()].name);
#endif // ----------------------------------------------------------------------
            importer.scene->instances.emplace_back(
                new TriMeshInstance{ {}, meshes[node.meshIndex.value()], nullptr, transform });
        }

        for (auto& childIndex : node.children) {
            ProcessNode(asset->nodes[childIndex], transform);
        }
    }
}