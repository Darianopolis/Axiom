if Project "axiom" then
    Compile "src/**"
    Include "src"
    Import {
        "nova",
        "meshoptimizer",
        "fastgltf",
        "stb",
    }
    Artifact { "out/main", type = "Console" }
end