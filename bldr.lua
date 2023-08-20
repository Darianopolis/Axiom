if Project "axiom" then
    Compile "src/**"
    Include "src"
    Import {
        "nova",
        "meshoptimizer",
        "fastgltf",
    }
    Artifact { "out/main", type = "Console" }
end