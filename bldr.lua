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
    }
    Artifact { "out/main", type = "Console" }
end