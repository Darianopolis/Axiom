#include <nova/rhi/nova_RHI.hpp>
#include <nova/imgui/vulkan/nova_VulkanImGui.hpp>

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#pragma warning(push)
#pragma warning(disable: 4189)
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>
#pragma warning(pop)

template<> struct fastgltf::ElementTraits<glm::vec4> : fastgltf::ElementTraitsBase<nova::types::f32, fastgltf::AccessorType::Vec4> {};
template<> struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<nova::types::f32, fastgltf::AccessorType::Vec3> {};
template<> struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<nova::types::f32, fastgltf::AccessorType::Vec2> {};

#include <meshoptimizer.h>

using namespace nova::types;

template<>
struct ankerl::unordered_dense::hash<glm::i64vec2> 
{
    using is_avalanching = void;
    
    uint64_t operator()(const glm::i64vec2& key) const noexcept {
        return detail::wyhash::hash(&key, sizeof(key));
    }
};

struct Application
{
    nova::Context       context;
    GLFWwindow*          window = nullptr;
    nova::Swapchain   swapchain;
    nova::Queue           queue;
    nova::CommandState    state;
    nova::Fence           fence;
    nova::CommandPool   cmdPool;
    nova::Shader   vertexShader;
    nova::Shader fragmentShader;
    nova::PipelineLayout layout;

// -----------------------------------------------------------------------------

    std::unique_ptr<nova::ImGuiLayer> imgui;

// -----------------------------------------------------------------------------

    u64                                           frames = 0;
    f32                                              fps = 0;
    std::chrono::steady_clock::time_point lastUpdateTime;
    std::chrono::steady_clock::time_point lastReportTime;

// -----------------------------------------------------------------------------

    Vec2U              extent{ };
    nova::Texture depthBuffer;

// -----------------------------------------------------------------------------


    f64 moveSpeed = 1.0;
    glm::dvec3 position = Vec3(0.f, 0.f, 2.f);

    bool lastMouseDrag = false;
    float   mouseSpeed = 0.0025f;
    POINT     savedPos;
    Quat      rotation = Vec3(0.f);

// -----------------------------------------------------------------------------

    struct Vertex
    {
        Vec3 position;

        static constexpr std::array Layout {
            nova::Member("position", nova::ShaderVarType::Vec3),
        };
    };

    nova::Buffer  indexBuffer;
    u32            indexCount;
    nova::Buffer vertexBuffer;
    u32           vertexCount;
    Vec3 color;

    struct Chunk_PC
    {
        Mat4    model;
        Mat4 viewProj;
        u64  vertices;
        Vec3    color;

        static constexpr std::array Layout {
            nova::Member("model",     nova::ShaderVarType::Mat4),
            nova::Member("viewProj",  nova::ShaderVarType::Mat4),
            nova::Member("vertices",  nova::ShaderVarType::U64),
            nova::Member("color",     nova::ShaderVarType::Vec3),
        };
    };

// -----------------------------------------------------------------------------

    struct ModelInfo
    {
        std::filesystem::path path;
        Mat4 transform = Mat4(1.f);
        std::string scene = "";
        std::shared_ptr<fastgltf::Asset> asset;
        u32 meshOffset;
    };
    
    struct Mesh 
    { 
        u32 indexOffset;
        u32 indexCount; 
        u32 maxVertex;
        nova::AccelerationStructure blas;
    };
    std::vector<Mesh> meshes;

    struct MeshInstance 
    {
        Mat4 position;
        u32 indexCount;
        u32 indexOffset;
    };
    std::vector<MeshInstance> instances;

// -----------------------------------------------------------------------------

    ~Application()
    {
        if (fence) {
            fence.Wait();
        }

        for (auto& mesh : meshes)
            mesh.blas.Destroy();
        indexBuffer.Destroy();
        vertexBuffer.Destroy();
        depthBuffer.Destroy();
        layout.Destroy();
        vertexShader.Destroy();
        fragmentShader.Destroy();
        cmdPool.Destroy();
        fence.Destroy();
        state.Destroy();
        swapchain.Destroy();
        imgui.reset();
        context.Destroy();
        if (window) {
            glfwDestroyWindow(window);
            glfwTerminate();
        }
    }

    void Init()
    {
        context = nova::Context::Create({
            .debug = true,
            .rayTracing = true,
        });

        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(1920, 1200, "Proverse", nullptr, nullptr);
        swapchain = nova::Swapchain::Create(context, glfwGetWin32Window(window),
            nova::TextureUsage::ColorAttach | nova::TextureUsage::Storage,
            nova::PresentMode::Mailbox);

        queue = context.GetQueue(nova::QueueFlags::Graphics, 0);
        state = nova::CommandState::Create(context);
        fence = nova::Fence::Create(context);
        cmdPool = nova::CommandPool::Create(context, queue);

        // Triangle shader and pipeline

        layout = nova::PipelineLayout::Create(context, 
            {{"pc", {Chunk_PC::Layout.begin(), Chunk_PC::Layout.end()}}},
            {}, nova::BindPoint::Graphics);

        vertexShader = nova::Shader::Create(context, nova::ShaderStage::Vertex, {
            nova::shader::Structure("Vertex", Vertex::Layout),
            nova::shader::Output("outPosition", nova::ShaderVarType::Vec3),
            nova::shader::Output("color", nova::ShaderVarType::Vec3),
            nova::shader::Layout(layout),
            nova::shader::Kernel(R"glsl(
                Vertex v = Vertex_BR(pc.vertices)[gl_VertexIndex];
                vec4 pos = pc.model * vec4(v.position, 1.0);
                outPosition = pos.xyz;
                gl_Position = pc.viewProj * pos;
            )glsl"),
        });

        fragmentShader = nova::Shader::Create(context, nova::ShaderStage::Fragment, {
            nova::shader::Input("inPosition", nova::ShaderVarType::Vec3, nova::ShaderInputFlags::PerVertex),
            nova::shader::Input("inColor", nova::ShaderVarType::Vec3),
            nova::shader::Output("outColor", nova::ShaderVarType::Vec4),
            nova::shader::Kernel(R"glsl(
                vec3 v01 = inPosition[1] - inPosition[0];
                vec3 v02 = inPosition[2] - inPosition[0];
                vec3 nrm = normalize(cross(v01, v02));
                if (!gl_FrontFacing)
                    nrm = -nrm;
                outColor = vec4(nrm * 0.5 + 0.5, 1.0);
            )glsl"),
        });

        {
            auto cmd = cmdPool.Begin(state);
            imgui = std::make_unique<nova::ImGuiLayer>(
                context, cmd, swapchain.GetFormat(), window, nova::ImGuiConfig {
                    .flags = ImGuiConfigFlags_DockingEnable
                });
            imgui->dockMenuBar = false;
            queue.Submit({cmd}, {}, {fence});
            fence.Wait();
        }

        LoadModel();
        // ConvertGlb();
    }

    void ConvertGlb()
    {
        auto path = "C:/Users/Darian/Downloads/ancient_world/ancient_world.glb";

        std::ifstream in(path, std::ios::binary | std::ios::in);
        std::ofstream out("scene.bin", std::ios::binary | std::ios::out);

        size_t offset = 12;
        in.seekg(offset);
        uint32_t length;
        in.read((char*)&length, 4);
        NOVA_LOGEXPR(length);
        size_t binBegin = 12 + 4 + 4 + length + 4 + 4;
        in.seekg(0, std::ios::end);
        size_t binEnd = in.tellg();
        NOVA_LOGEXPR(binEnd - binBegin);
        in.seekg(binBegin);
        std::vector<char> binary;
        binary.resize(binEnd - binBegin);
        in.read(binary.data(), binEnd - binBegin);

        out.write(binary.data(), binEnd - binBegin);
        out.flush();
    }

    void LoadModel()
    {
        std::vector<ModelInfo> modelInfos{
            // ModelInfo{ "D:/Dev/Data/3DModels/Bistro/exterior.gltf" },
            // ModelInfo{ "D:/Dev/Data/3DModels/Bistro/interior.gltf" },
            ModelInfo{ "assets/models/ancient-valley/scene.gltf" },
            // ModelInfo{ "D:/Dev/Projects/pyrite/pyrite-v4/assets/models/deccer-cubes/SM_Deccer_Cubes_Textured_Complex.gltf" },
            // ModelInfo{ "D:/Dev/Projects/pyrite/pyrite-v4/assets/models/matrix_city/matrix_city.glb" },
            // ModelInfo{ "D:/Dev/Projects/pyrite/pyrite-v4/assets/models/Sponza/NewSponza_4_Combined_glTF.gltf" },
            // ModelInfo{ "D:/Dev/Projects/pyrite/pyrite-v4/assets/models/Sponza/NewSponza_Curtains_glTF.gltf" },
            // ModelInfo{ "D:/Dev/Projects/pyrite/pyrite-v4/assets/models/Sponza/NewSponza_IvyGrowth_glTF.gltf" },
            // ModelInfo{ "D:/Dev/Projects/pyrite/pyrite-v4/assets/models/Sponza/NewSponza_Main_Blender_glTF.gltf" },
        };

NOVA_DEBUG();
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

            for (auto& info : modelInfos) {
                fastgltf::GltfDataBuffer data;
                data.loadFromFile(info.path);
                auto baseDir = info.path.parent_path();

NOVA_DEBUG();
                constexpr auto gltfOptions =
                    fastgltf::Options::DontRequireValidAssetMember
                    | fastgltf::Options::AllowDouble
                    | fastgltf::Options::LoadGLBBuffers
                    | fastgltf::Options::LoadExternalBuffers;
                
NOVA_DEBUG();
                info.asset = std::make_shared<fastgltf::Asset>(std::move(
                    fastgltf::determineGltfFileType(&data) == fastgltf::GltfType::glTF
                        ? parser.loadGLTF(&data, baseDir, gltfOptions).get()
                        : parser.loadBinaryGLTF(&data, baseDir, gltfOptions).get()));
NOVA_DEBUG();
            }
        }

// -----------------------------------------------------------------------------

NOVA_DEBUG();
        usz totalVertexCount = 0;
        usz totalIndexCount = 0;

        for (auto& info : modelInfos) {
            info.meshOffset = u32(meshes.size());
            for (auto& mesh : info.asset->meshes) {
                u32 meshIndexOffset = u32(totalIndexCount);
                for (auto& prim : mesh.primitives) {
                    if (prim.findAttribute("POSITION") == prim.attributes.end() || !prim.indicesAccessor.has_value())
                        continue;

                    auto& positionAccessor = info.asset->accessors[prim.findAttribute("POSITION")->second];
                    auto& indexAccessor = info.asset->accessors[prim.indicesAccessor.value()];

                    totalVertexCount += positionAccessor.count;
                    totalIndexCount += indexAccessor.count;
                }
                
                meshes.emplace_back(meshIndexOffset, u32(totalIndexCount - meshIndexOffset));
            }
        }

NOVA_DEBUG();
        NOVA_LOGEXPR(totalVertexCount);
        if (totalVertexCount > UINT32_MAX)
            NOVA_THROW("Vertex count too large");
        vertexCount = u32(totalVertexCount);

NOVA_DEBUG();
        NOVA_LOGEXPR(totalIndexCount);
        if (totalIndexCount > UINT32_MAX)
            NOVA_THROW("Index count too large");
        indexCount = u32(totalIndexCount);

NOVA_DEBUG();
// -----------------------------------------------------------------------------

        vertexBuffer = nova::Buffer::Create(context, sizeof(Vertex) * vertexCount,
            nova::BufferUsage::Storage 
            | nova::BufferUsage::AccelBuild
            | nova::BufferUsage::TransferDst,
            nova::BufferFlags::DeviceLocal);
        
NOVA_DEBUG();
        indexBuffer = nova::Buffer::Create(context, sizeof(u32) * indexCount,
            nova::BufferUsage::Index 
            | nova::BufferUsage::AccelBuild
            | nova::BufferUsage::TransferDst,
            nova::BufferFlags::DeviceLocal);

NOVA_DEBUG();
        auto staging = nova::Buffer::Create(context, std::max(vertexBuffer.GetSize(), indexBuffer.GetSize()),
            nova::BufferUsage::TransferSrc,
            nova::BufferFlags::Mapped);
        NOVA_ON_SCOPE_EXIT(&) { staging.Destroy(); };
NOVA_DEBUG();

// -----------------------------------------------------------------------------

        
        {
            auto vertices = reinterpret_cast<Vertex*>(staging.GetMapped());
            u32 vertexOffset = 0;

            for (auto& info : modelInfos) {
                for (auto& mesh : info.asset->meshes) {
                    for (auto& prim : mesh.primitives) {
                        if (prim.findAttribute("POSITION") == prim.attributes.end() || !prim.indicesAccessor.has_value())
                            continue;

                        auto& positionAccessor = info.asset->accessors[prim.findAttribute("POSITION")->second];

                        fastgltf::iterateAccessorWithIndex<Vec3>(*info.asset, positionAccessor, [&](Vec3 pos, usz index) {
                            vertices[vertexOffset + index].position = pos;
                        }, fastgltf::DefaultBufferDataAdapter{});

                        vertexOffset += u32(positionAccessor.count);
                    }
                }
            }
NOVA_DEBUG();

            auto cmd = cmdPool.Begin(state);
            cmd.CopyToBuffer(vertexBuffer, staging, vertexBuffer.GetSize());
            queue.Submit({cmd}, {}, {fence});
            fence.Wait();
        }

NOVA_DEBUG();
// -----------------------------------------------------------------------------

        {

            auto indices = reinterpret_cast<u32*>(staging.GetMapped());
            u32 vertexOffset = 0;
            u32 indexOffset = 0;
            u32 meshIndex = 0;

            for (auto& info : modelInfos) {
                for (auto& mesh : info.asset->meshes) {
                    u32 highestVertexOffset = 0;
                    for (auto& prim : mesh.primitives) {
                        if (prim.findAttribute("POSITION") == prim.attributes.end() || !prim.indicesAccessor.has_value())
                            continue;

                        auto& positionAccessor = info.asset->accessors[prim.findAttribute("POSITION")->second];
                        auto& indexAccessor = info.asset->accessors[prim.indicesAccessor.value()];

                        fastgltf::iterateAccessorWithIndex<u32>(*info.asset, indexAccessor, [&](u32 vertexIndex, usz index) {
                            indices[indexOffset + index] = u32(vertexOffset + vertexIndex);
                            highestVertexOffset = std::max(highestVertexOffset, u32(vertexOffset + vertexIndex));
                        });

                        vertexOffset += u32(positionAccessor.count);
                        indexOffset += u32(indexAccessor.count);
                    }

                    meshes[meshIndex++].maxVertex = highestVertexOffset;
                }
            }
NOVA_DEBUG();

            auto cmd = cmdPool.Begin(state);
            cmd.CopyToBuffer(indexBuffer, staging, indexBuffer.GetSize());
            queue.Submit({cmd}, {}, {fence});
            fence.Wait();
        }
NOVA_DEBUG();
// -----------------------------------------------------------------------------

        {
            auto builder = nova::AccelerationStructureBuilder::Create(context);
            NOVA_ON_SCOPE_EXIT(&) { builder.Destroy(); };
            auto scratch = nova::Buffer::Create(context, 0, nova::BufferUsage::Storage, nova::BufferFlags::DeviceLocal);
            NOVA_ON_SCOPE_EXIT(&) { scratch.Destroy(); };
            usz totalBlasSizes = 0;
            for (auto& mesh : meshes) {
                NOVA_LOG("Mesh, indices = {}, maxVertex = {}", mesh.indexCount, mesh.maxVertex);
                builder.SetTriangles(0,
                    vertexBuffer.GetAddress(), nova::Format::RGB32_SFloat, sizeof(Vertex), mesh.maxVertex,
                    indexBuffer.GetAddress()/* + (sizeof(u32) * mesh.indexOffset)*/, nova::IndexType::U32, mesh.indexCount / 3);
                builder.Prepare(nova::AccelerationStructureType::BottomLevel,
                    nova::AccelerationStructureFlags::AllowCompaction
                    | nova::AccelerationStructureFlags::PreferFastTrace,
                    1);
                scratch.Resize(builder.GetBuildScratchSize());
                NOVA_LOG("Build, size = {}, scratch = {}", nova::ByteSizeToString(builder.GetBuildSize()), nova::ByteSizeToString(builder.GetBuildScratchSize()));
                totalBlasSizes += builder.GetBuildSize();

                auto cmd = cmdPool.Begin(state);
                mesh.blas = nova::AccelerationStructure::Create(context, builder.GetBuildSize(), nova::AccelerationStructureType::BottomLevel);
                cmd.BuildAccelerationStructure(builder, mesh.blas, scratch);
                queue.Submit({cmd}, {}, {fence});
                fence.Wait();
                NOVA_LOG("Compact size = {}", nova::ByteSizeToString(builder.GetCompactSize()));
            }
            NOVA_LOGEXPR(nova::ByteSizeToString(totalBlasSizes));
        }

// -----------------------------------------------------------------------------

        for (auto& info : modelInfos) {
            auto& scene = info.asset->scenes[info.asset->defaultScene.value_or(0)];
            for (auto& rootIndex : scene.nodeIndices) {
                auto process = [&](this auto self, fastgltf::Node& node, Mat4 parentTransform) -> void {
                    Mat4 transform = Mat4(1.f);
                    if (auto _trs = std::get_if<fastgltf::Node::TRS>(&node.transform)) {
                        auto& trs = *_trs;
                        auto translation = std::bit_cast<Vec3>(trs.translation);
                        auto rotation = Quat(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
                        auto scale = std::bit_cast<glm::vec3>(trs.scale);
                        transform = glm::translate(Mat4(1.f), translation)
                            * glm::mat4_cast(rotation)
                            * glm::scale(Mat4(1.f), scale);
                    } else if (auto m = std::get_if<fastgltf::Node::TransformMatrix>(&node.transform)) {
                        transform = std::bit_cast<Mat4>(*m);
                    }

                    // (void)parentTransform;
                    transform = parentTransform * transform;

                    if (node.meshIndex.has_value()) {
                        auto meshPair = meshes[info.meshOffset + node.meshIndex.value()];
                        instances.push_back(MeshInstance{
                            // .position = Mat4(1.f),
                            .position = transform,
                            .indexCount = meshPair.indexCount,
                            .indexOffset = meshPair.indexOffset,
                        });
                    }

                    for (auto& childIndex : node.children) {
                        self(info.asset->nodes[childIndex], transform);
                    }
                };

                process(info.asset->nodes[rootIndex], Mat4(1.f));
            }
        }

        NOVA_LOGEXPR(instances.size());
    }

    static Mat4 ProjInfReversedZRH(f32 fovY, f32 aspectWbyH, f32 zNear)
    {
        // https://nlguillemot.wordpress.com/2016/12/07/reversed-z-in-opengl/

        f32 f = 1.f / glm::tan(fovY / 2.f);
        Mat4 proj{};
        proj[0][0] = f / aspectWbyH;
        proj[1][1] = f;
        proj[3][2] = zNear; // Right, middle-bottom
        proj[2][3] = -1.f;  // Bottom, middle-right

        return proj;
    }

    void Draw()
    {
        // Wait for previous frame

        fence.Wait();

        using namespace std::chrono;
        auto now = steady_clock::now();
        auto timeStep = duration_cast<duration<f32>>(now - lastUpdateTime).count();
        lastUpdateTime = now;

        // FPS

        frames++;
        if (now - lastReportTime > 1s)
        {
            fps = frames / duration_cast<duration<f32>>(now - lastReportTime).count();
            lastReportTime = now;
            frames = 0;
        }

        // Camera

        {
            glm::dvec3 translate = {};
            if (glfwGetKey(window, GLFW_KEY_W))          translate += glm::dvec3( 0.f,  0.f, -1.f);
            if (glfwGetKey(window, GLFW_KEY_A))          translate += glm::dvec3(-1.f,  0.f,  0.f);
            if (glfwGetKey(window, GLFW_KEY_S))          translate += glm::dvec3( 0.f,  0.f,  1.f);
            if (glfwGetKey(window, GLFW_KEY_D))          translate += glm::dvec3( 1.f,  0.f,  0.f);
            if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)) translate += glm::dvec3( 0.f, -1.f,  0.f);
            if (glfwGetKey(window, GLFW_KEY_SPACE))      translate += glm::dvec3( 0.f,  1.f,  0.f);
            if (translate.x || translate.y || translate.z) {
                position += glm::dquat(rotation) * (glm::normalize(translate) * moveSpeed * f64(timeStep));
            }
            position.y = glm::clamp(position.y, -10000.0, 10000.0);
        }

        {
            Vec2 delta = {};
            if (GetFocus() == glfwGetWin32Window(window) && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_2)) {
                POINT p;
                GetCursorPos(&p);
                LONG dx = p.x - savedPos.x;
                LONG dy = p.y - savedPos.y;
                if (lastMouseDrag) {
                    delta = { f32(dx), f32(dy) };
                }
                else {
                    GetCursorPos(&savedPos);
                    ShowCursor(false);
                    lastMouseDrag = true;
                }
                SetCursorPos(savedPos.x, savedPos.y);
            }
            else if (lastMouseDrag) {
                ShowCursor(true);
                lastMouseDrag = false;
            }

            if ((delta.x || delta.y) && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_2)) {
                rotation = glm::angleAxis(delta.x * mouseSpeed, Vec3(0.f, -1.f, 0.f)) * rotation;
                rotation = rotation * glm::angleAxis(delta.y * mouseSpeed, Vec3(-1.f, 0.f, 0.f));
                rotation = glm::normalize(rotation);
            }
        }

        int cWidth, cHeight;
        glfwGetFramebufferSize(window, &cWidth, &cHeight);
        auto proj = ProjInfReversedZRH(glm::radians(90.f), f32(cWidth) / cHeight, 0.01f);
        // auto viewProj = proj * glm::mat4_cast(glm::inverse(rotation));
        auto view = glm::affineInverse(glm::translate(Mat4(1.f), glm::vec3(position)) * glm::mat4_cast(rotation));
        auto viewProj = proj * view;

        // Record

        queue.Acquire({swapchain}, {fence});
        if (!depthBuffer || extent != swapchain.GetExtent()) {
            extent = swapchain.GetExtent();
            depthBuffer.Destroy();
            depthBuffer = nova::Texture::Create(context, 
                Vec3U(extent, 0), 
                nova::TextureUsage::DepthStencilAttach, 
                nova::Format::D32_SFloat);
        }
        cmdPool.Reset();
        auto cmd = cmdPool.Begin(state);
        cmd.BeginRendering({{}, extent}, {swapchain.GetCurrent()}, depthBuffer);
        cmd.SetGraphicsState(layout, {vertexShader, fragmentShader}, {
            .cullMode = nova::CullMode::None,
            // .polyMode = nova::PolygonMode::Line,
            .depthEnable = true,
            .flipVertical = true,
        });
        cmd.ClearColor(0, Vec4(Vec3(0.1f), 1.f), extent);
        cmd.ClearDepth(0.f, extent);
        cmd.BindIndexBuffer(indexBuffer, nova::IndexType::U32);
        size_t triangles = 0;
        {
            // std::scoped_lock lock{ chunkLock };

            // for (auto& instance : instances) {
            for (u32 i = 0; i < instances.size(); ++i) {
                auto& instance = instances[i];
                triangles += instance.indexCount / 3;
            //     if (!chunk) {
            //         continue;
            //     }

            //     f64 chunkSize = noise.len * noise.size;
            //     auto worldPos = glm::dvec3(
            //         index.x * chunkSize, 
            //         0.0,
            //         index.y * chunkSize);

                cmd.PushConstants(layout, 0, sizeof(Chunk_PC), nova::Temp(Chunk_PC {
                    // .position = worldPos - position,
                    .model = instance.position,
                    .viewProj = viewProj,
                    .vertices = vertexBuffer.GetAddress(),
                    // .amplitude = noise.amplitude,
                    .color = color,
                    // .size = noise.size,
                    // .len = noise.len,
                }));
                cmd.DrawIndexed(instance.indexCount, 1, instance.indexOffset, 0, 0);
                // cmd.DrawIndexed(u32(indexCount), 1, 0, 0, 0);
            }
        }
        cmd.EndRendering();

        imgui->BeginFrame();
        if (ImGui::Begin("Settings")) {
            NOVA_ON_SCOPE_EXIT() { ImGui::End(); };

            ImGui::Text("Position: (%.2f, %.2f, %.2f)", position.x, position.y, position.z);
            ImGui::Text("Speed: %.2f", moveSpeed);
            ImGui::Text("Triangles: %lli", triangles);
            ImGui::Text("FPS: %.2f", fps);
        }
        imgui->DrawFrame(cmd, swapchain.GetCurrent());

        cmd.Present(swapchain);
        queue.Submit({cmd}, {fence}, {fence});
        queue.Present({swapchain}, {fence});
    }

    void Run()
    {
        lastUpdateTime = std::chrono::steady_clock::now();
        lastReportTime = std::chrono::steady_clock::now();

        glfwSetWindowUserPointer(window, this);
        glfwSetWindowSizeCallback(window, [](auto w, int,int) {
            static_cast<decltype(this)>(glfwGetWindowUserPointer(w))->Draw();
        });

        glfwSetScrollCallback(window, [](auto w, f64, f64 dy) {
            auto& self = *static_cast<decltype(this)>(glfwGetWindowUserPointer(w));
            {
                if (dy > 0) self.moveSpeed *= 1.5f;
                if (dy < 0) self.moveSpeed /= 1.5f;
            }
        });

        while (!glfwWindowShouldClose(window)) {
            Draw();
            glfwPollEvents();
        }
    }
};

int main()
{
    try {
        Application app;
        app.Init();
        app.Run();
    }
    catch (const std::exception& e) {
        NOVA_LOG("Error - {}", e.what());
    }
    catch (fastgltf::Error e) {
        NOVA_LOG("Error - {}", i32(e));
    }
    catch (...) {
        NOVA_LOG("Error - ...");
    }
}