if Project "axiom" then
    Compile "src/**"
    Include "src"
    Import {
        "nova",
        "meshoptimizer",
        "stb",
        "bc7enc",
        "assimp",
        "fastgltf",
        "fast-obj",
        "ufbx",
        "base64",
    }
end

if Project "axiom-engine" then
    Compile "test/engine_test.cpp"
    Import "axiom"
    Artifact { "out/engine", type = "Console" }
end

if Project "axiom-render" then
    Compile "test/render_test.cpp"
    Import "axiom"
    Artifact { "out/render", type = "Console" }
end