if Project "axiom" then
    Compile "src/**"
    Include "src"
    Import {
        "nova",
        "meshoptimizer",
        "fastgltf",
        "stb",
        "assimp",
    }
    Artifact { "out/main", type = "Console" }
end