// forge_shaderc: compiles one GLSL shader for every GPU backend at build time
// and writes a C++ header with the results (see forge/render/shader_blob.h).
//
//   forge_shaderc <vert|frag|comp> <input.glsl> <output.h> <name>
//
// GLSL -> SPIR-V (glslang) for Vulkan. On Windows also SPIR-V -> HLSL
// (SPIRV-Cross) -> DXBC (the system D3DCompiler) for Direct3D 12. The HLSL is
// written next to the header for debugging.
//
// Resource layout follows SDL_GPU's rules: vertex shaders use set 0 for
// textures and storage buffers and set 1 for uniform buffers; fragment
// shaders use sets 2 and 3. Compute shaders: set 0 for sampled textures and
// read-only storage, set 1 for read-write storage, set 2 for uniform buffers.

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_hlsl.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <d3dcompiler.h>
#endif

namespace {

bool read_text(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool compile_spirv(const std::string& source, EShLanguage stage, const char* path, std::vector<unsigned>& spirv) {
    glslang::TShader shader(stage);
    const char* text = source.c_str();
    const char* names[] = {path};
    shader.setStringsWithLengthsAndNames(&text, nullptr, names, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        std::fprintf(stderr, "%s\n%s\n", shader.getInfoLog(), shader.getInfoDebugLog());
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        std::fprintf(stderr, "%s\n%s\n", program.getInfoLog(), program.getInfoDebugLog());
        return false;
    }
    glslang::SpvOptions options;
    options.validate = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
    return !spirv.empty();
}

void write_bytes(std::ostream& out, const char* name, const unsigned char* data, size_t size) {
    out << "inline constexpr unsigned char " << name << "[] = {";
    for (size_t i = 0; i < size; ++i) {
        if (i % 24 == 0) out << "\n    ";
        out << static_cast<unsigned>(data[i]) << ',';
    }
    out << "\n};\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: forge_shaderc <vert|frag|comp> <input.glsl> <output.h> <name>\n");
        return 2;
    }
    const bool vertex = std::strcmp(argv[1], "vert") == 0;
    const bool compute = std::strcmp(argv[1], "comp") == 0;
    if (!vertex && !compute && std::strcmp(argv[1], "frag") != 0) {
        std::fprintf(stderr, "forge_shaderc: unknown stage %s\n", argv[1]);
        return 2;
    }
    const EShLanguage stage = vertex ? EShLangVertex : (compute ? EShLangCompute : EShLangFragment);
    const char* stage_name = vertex ? "Vertex" : (compute ? "Compute" : "Fragment");
    const char* profile = vertex ? "vs_5_1" : (compute ? "cs_5_1" : "ps_5_1");
    const char* input = argv[2];
    const std::string output = argv[3];
    const std::string name = argv[4];

    std::string source;
    if (!read_text(input, source)) {
        std::fprintf(stderr, "forge_shaderc: cannot read %s\n", input);
        return 1;
    }

    glslang::InitializeProcess();
    std::vector<unsigned> spirv;
    const bool ok = compile_spirv(source, stage, input, spirv);
    glslang::FinalizeProcess();
    if (!ok) return 1;

    // Resource counts SDL needs when creating the shader.
    spirv_cross::CompilerHLSL hlsl(spirv);
    const spirv_cross::ShaderResources res = hlsl.get_shader_resources();
    const size_t samplers = res.sampled_images.size();
    size_t storage_textures = res.storage_images.size();
    size_t storage_buffers = res.storage_buffers.size();
    const size_t uniform_buffers = res.uniform_buffers.size();
    // Compute shaders tell SDL read-only and read-write storage apart.
    size_t rw_textures = 0, rw_buffers = 0;
    unsigned threads[3] = {0, 0, 0};
    if (compute) {
        storage_textures = storage_buffers = 0;
        for (const auto& r : res.storage_images) {
            if (hlsl.get_decoration(r.id, spv::DecorationNonWritable)) ++storage_textures;
            else ++rw_textures;
        }
        for (const auto& r : res.storage_buffers) {
            if (hlsl.get_buffer_block_flags(r.id).get(spv::DecorationNonWritable)) ++storage_buffers;
            else ++rw_buffers;
        }
        for (unsigned i = 0; i < 3; ++i) threads[i] = hlsl.get_execution_mode_argument(spv::ExecutionModeLocalSize, i);
    }

    // HLSL for Direct3D 12 (shader model 5.1: register spaces = SPIR-V sets).
    spirv_cross::CompilerHLSL::Options hlsl_options;
    hlsl_options.shader_model = 51;
    // Draws always start at instance 0; without this SPIRV-Cross adds a hidden
    // constant buffer that SDL does not bind.
    hlsl_options.support_nonzero_base_vertex_base_instance = false;
    hlsl.set_hlsl_options(hlsl_options);
    const std::string hlsl_source = hlsl.compile();
    {
        std::ofstream f(output + ".hlsl", std::ios::binary);
        f << hlsl_source;
    }

    std::vector<unsigned char> dxbc;
#if defined(_WIN32)
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(hlsl_source.data(), hlsl_source.size(), input, nullptr, nullptr, "main",
                                  profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::fprintf(stderr, "forge_shaderc: HLSL compile failed for %s\n%s\n", input,
                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        return 1;
    }
    const auto* bytes = static_cast<const unsigned char*>(code->GetBufferPointer());
    dxbc.assign(bytes, bytes + code->GetBufferSize());
    code->Release();
    if (errors) errors->Release();
#endif

    std::ostringstream out;
    out << "// Generated by forge_shaderc from " << input << ". Do not edit.\n#pragma once\n\n"
        << "#include \"forge/render/shader_blob.h\"\n\nnamespace forge::shaders {\n\n";
    write_bytes(out, (name + "_spirv").c_str(), reinterpret_cast<const unsigned char*>(spirv.data()),
                spirv.size() * sizeof(unsigned));
    if (!dxbc.empty()) write_bytes(out, (name + "_dxbc").c_str(), dxbc.data(), dxbc.size());
    out << "\ninline constexpr render::ShaderBlob " << name << " = {\n"
        << "    \"" << name << "\",\n"
        << "    render::ShaderStage::" << stage_name << ",\n"
        << "    " << name << "_spirv, sizeof(" << name << "_spirv),\n";
    if (dxbc.empty()) out << "    nullptr, 0,\n";
    else out << "    " << name << "_dxbc, sizeof(" << name << "_dxbc),\n";
    out << "    " << samplers << ", " << storage_textures << ", " << storage_buffers << ", " << uniform_buffers << ",\n"
        << "    " << rw_textures << ", " << rw_buffers << ", " << threads[0] << ", " << threads[1] << ", " << threads[2]
        << ",\n};\n\n} // namespace forge::shaders\n";

    std::ofstream f(output, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "forge_shaderc: cannot write %s\n", output.c_str());
        return 1;
    }
    f << out.str();
    return 0;
}
