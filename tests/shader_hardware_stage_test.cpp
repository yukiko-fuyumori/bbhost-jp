// Synthetic containers only: the Shdr slot alone cannot distinguish an
// ordinary vertex shader from a domain shader needing tessellation context.
#include "gcn/container.h"
#include "gcn/isa.h"
#include "gcn/translate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using Stage = gcn::ShaderHardwareStage;
constexpr std::size_t kHeader = 16, kRegisters = 64;

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned k = 0; k < 4; ++k) bytes[at + k] = static_cast<std::uint8_t>(value >> (8 * k));
}

std::vector<std::uint8_t> container(std::uint8_t type, Stage stage,
                                  const std::vector<std::uint32_t>& words = {0xbf810000u}) {
    const std::size_t footer = kHeader + kRegisters + words.size() * 4;
    std::vector<std::uint8_t> bytes(footer + 28, 0);
    std::memcpy(bytes.data(), "Shdr", 4);
    bytes[8] = type;
    bytes[9] = kRegisters / 4;
    for (std::size_t k = 0; k < words.size(); ++k) put32(bytes, kHeader + kRegisters + k * 4, words[k]);
    std::memcpy(bytes.data() + footer, "OrbShdr", 7);
    bytes[footer + 7] = 7;
    put32(bytes, footer + 8, static_cast<std::uint32_t>(words.size() * 4) << 8 |
                                 static_cast<std::uint32_t>(stage) << 2 | 0x43u);
    return bytes;
}

gcn::ShaderCode parse(const std::vector<std::uint8_t>& bytes) {
    gcn::ShaderCode code;
    CHECK(gcn::shader_code(bytes, code));
    return code;
}
}  // namespace

int main() {
    // Same Shdr type and identical instructions, but a distinct binary role.
    const auto vertex = parse(container(1, Stage::Vertex));
    const auto domain = parse(container(1, Stage::DomainVertex));
    CHECK(vertex.type == domain.type && vertex.words == domain.words);
    CHECK(vertex.hardware_stage == Stage::Vertex && !gcn::shader_needs_tessellation_context(vertex));
    CHECK(domain.hardware_stage == Stage::DomainVertex && gcn::shader_needs_tessellation_context(domain));
    // The domain role must be recognized even when this particular program
    // has no LDS access. Its invocation inputs still require draw context.
    CHECK(domain.words.size() == 1);

    struct Role { std::uint8_t type; Stage stage; bool context; };
    const Role roles[] = {
        {2, Stage::Pixel, false}, {1, Stage::Vertex, false}, {5, Stage::Export, false},
        {6, Stage::Local, false}, {4, Stage::Compute, false}, {3, Stage::Geometry, false},
        {1, Stage::GeometryVertex, false}, {7, Stage::Hull, false},
        {1, Stage::DomainVertex, true}, {5, Stage::DomainExport, true},
    };
    for (const auto& role : roles) {
        const auto code = parse(container(role.type, role.stage));
        CHECK(code.hardware_stage == role.stage);
        CHECK(gcn::shader_needs_tessellation_context(code) == role.context);
    }

    // Source language/cache flags occupy the other bits of the same byte.
    for (unsigned language = 0; language < 4; ++language) {
        for (unsigned flags = 0; flags < 4; ++flags) {
            auto bytes = container(1, Stage::DomainVertex);
            bytes[bytes.size() - 28 + 8] = static_cast<unsigned>(Stage::DomainVertex) << 2 | language << 6 | flags;
            CHECK(gcn::shader_needs_tessellation_context(parse(bytes)));
        }
    }

    // A genuine ordinary VS using addressable LDS must remain a translation
    // failure. Classifying every DS instruction as tessellation would hide it.
    const std::vector<std::uint32_t> lds = {0xd8000000u | (54u << 18), 1u << 24, 0xbf810000u};
    const auto ordinary_lds = parse(container(1, Stage::Vertex, lds));
    CHECK(!gcn::shader_needs_tessellation_context(ordinary_lds));
    const auto program = gcn::decode(ordinary_lds.words.data(), ordinary_lds.words.size());
    CHECK(program.errors.empty());
    gcn::TranslateOptions options;
    options.stage = gcn::Stage::Vertex;
    const auto translated = gcn::translate(program, options);
    CHECK(!translated.ok());
    CHECK(std::any_of(translated.errors.begin(), translated.errors.end(), [](const std::string& error) {
        return error.find("LDS use outside a compute shader") != std::string::npos;
    }));

    for (const auto bytes : {container(2, Stage::DomainVertex), container(5, Stage::DomainVertex),
                             container(1, Stage::DomainExport), container(1, static_cast<Stage>(15))}) {
        const auto code = parse(bytes);
        CHECK(code.hardware_stage == Stage::Unknown && !gcn::shader_needs_tessellation_context(code));
    }
    auto bytes = container(1, Stage::DomainVertex);
    const std::size_t footer = bytes.size() - 28;
    bytes[footer + 7] = 8;  // unknown metadata version
    CHECK(parse(bytes).hardware_stage == Stage::Unknown);
    bytes = container(1, Stage::DomainVertex);
    bytes[9] = 0;  // no stage register block
    CHECK(parse(bytes).hardware_stage == Stage::Unknown);
    bytes = container(1, Stage::DomainVertex);
    for (std::size_t size = footer; size < footer + 12; ++size) {
        gcn::ShaderCode code;
        CHECK(!gcn::shader_code(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + size), code));
        CHECK(!gcn::shader_needs_tessellation_context(code));
    }
    for (std::size_t size = footer + 12; size < footer + 28; ++size) {
        const auto code = parse(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + size));
        CHECK(code.hardware_stage == Stage::Unknown && !gcn::shader_needs_tessellation_context(code));
    }

    gcn::ShaderCode reused = domain;
    CHECK(!gcn::shader_code({}, reused));
    CHECK(reused.hardware_stage == Stage::Unknown && !gcn::shader_needs_tessellation_context(reused));
    for (const std::uint32_t length : {0u, 3u, 1024u}) {
        bytes = container(1, Stage::DomainVertex);
        put32(bytes, footer + 8, length << 8 | 0x63u);
        CHECK(!gcn::shader_code(bytes, reused));
        CHECK(!gcn::shader_needs_tessellation_context(reused));
    }
    bytes = container(1, Stage::DomainVertex);
    bytes[footer] = 'X';
    CHECK(!gcn::shader_code(bytes, reused));
    CHECK(!gcn::shader_needs_tessellation_context(reused));

    if (failures) return 1;
    std::puts("shader_hardware_stage_test: domain context, ordinary VS LDS errors and malformed metadata: passed");
}
