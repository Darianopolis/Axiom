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
    Artifact { "out/main", type = "Console" }
end