# Axiom

Personal asset management and rendering engine project

# Roadmap

- Asset importing
    - Interactive import process?
    - Import scripts?
    - Support more texture formats/comprsesion
        - DDS
        - BC7 (entropy optimized for super compression?)
    - Custom scene format
        - High level (fault-tolerant?) scene format
        - Low level specialized format "ready for rendering"
- Path tracing
    - Specular sampling
    - Thin surface effects
        - Diffusion
        - Subsurface
    - Volume boundary effects
        - Diffraction
        - Subsurface
    - Volume scattering effects
    - Light sampling
        - MIS
        - ReSTIR (GI)
    - Denoising
        - spatio-temporal reprojection
        - svgf
        - ReBLUR
