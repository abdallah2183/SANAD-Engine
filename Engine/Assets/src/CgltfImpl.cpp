// CgltfImpl.cpp — the single translation unit that compiles the vendored
// cgltf implementation (glTF 2.0 parser).
//
// GltfImport.cpp includes the same header WITHOUT the implementation macro
// for the declarations; this TU provides the definitions.

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
