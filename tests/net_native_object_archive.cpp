#include "net/native_object_archive.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_bounded_input_validation()
{
    using namespace kraken::net;

    CHECK(validate_native_object_archive(ByteView{}) ==
          NativeObjectArchiveErrorCode::EmptyInput);

    std::vector<Byte> exact(kNativeObjectArchiveMaxBytes, Byte{0x41});
    CHECK(validate_native_object_archive(ByteView{exact}) ==
          NativeObjectArchiveErrorCode::None);
    exact.push_back(Byte{0x42});
    CHECK(validate_native_object_archive(ByteView{exact}) ==
          NativeObjectArchiveErrorCode::InputTooLarge);
}

void test_temp_path_validation()
{
    using namespace kraken::net;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path().lexically_normal();

    CHECK(validate_native_object_archive_temp_path({}, root) ==
          NativeObjectArchiveErrorCode::InvalidTempPath);
    CHECK(validate_native_object_archive_temp_path(
              root / ".." / "escape.xml", root) ==
          NativeObjectArchiveErrorCode::PathTraversal);
    std::string root_text = root.string();
    while (!root_text.empty() &&
           (root_text.back() == '\\' || root_text.back() == '/'))
        root_text.pop_back();
    const std::filesystem::path trimmed_root(root_text);
    CHECK(validate_native_object_archive_temp_path(
              trimmed_root.parent_path() / "escape.xml", trimmed_root) ==
          NativeObjectArchiveErrorCode::PathTraversal);
    CHECK(validate_native_object_archive_temp_path(
              root / "kraken-safe.xml", root) ==
          NativeObjectArchiveErrorCode::None);
}

void test_temp_file_raii()
{
    using namespace kraken::net;
    std::filesystem::path path;
    {
        NativeObjectArchiveTempFile temp_file;
        CHECK(temp_file.create() == NativeObjectArchiveErrorCode::None);
        CHECK(temp_file.valid());
        path = temp_file.path();
        CHECK(!path.empty());
        CHECK(std::filesystem::is_regular_file(path));
    }
    CHECK(!path.empty());
    CHECK(!std::filesystem::exists(path));
}

kraken::net::NativeObjectArchiveV2 archive_fixture()
{
    using namespace kraken::net;
    NativeObjectArchiveV2 value{};
    value.map_namespace = "efa/r1m1";
    value.resource_fingerprint = "rfp2-sha256:test";
    const std::string xml =
        "<Vehicle Prototype=\"vehicle/prototype\"><Chassis "
        "Resource=\"mesh/chassis\"/><Runtime><Visibility enabled=\"1\"/>"
        "</Runtime></Vehicle>";
    value.canonical_xml.assign(reinterpret_cast<const Byte*>(xml.data()),
                               reinterpret_cast<const Byte*>(xml.data() +
                                                              xml.size()));
    const auto add_manifest = [&value](const char* path, const char* parent,
                                       const char* resource,
                                       const char* prototype,
                                       const NativeObjectArchiveResourceKind kind) {
        NativeObjectArchiveManifestEntry entry{
            path, parent, resource, prototype, kind, 0};
        entry.resource_fingerprint =
            native_object_archive_resource_identity_digest(
                value.resource_fingerprint, entry.archive_path,
                entry.resource_name, entry.prototype_name);
        value.manifest.push_back(std::move(entry));
    };
    add_manifest("Vehicle", "", "", "vehicle/prototype",
                 NativeObjectArchiveResourceKind::Prototype);
    add_manifest("Vehicle/Chassis", "Vehicle", "mesh/chassis", "",
                 NativeObjectArchiveResourceKind::Mesh);
    add_manifest("Vehicle/Runtime", "Vehicle", "", "",
                 NativeObjectArchiveResourceKind::Prototype);
    add_manifest("Vehicle/Runtime/Visibility", "Vehicle/Runtime", "", "",
                 NativeObjectArchiveResourceKind::Prototype);
    value.visual_runtime.push_back({"Vehicle/Runtime/Visibility",
                                    NativeObjectArchiveVisualRuntimeKind::Visibility,
                                    {}, {}, 0, true});
    return value;
}

void test_v2_round_trip_and_canonical_identity_exclusion()
{
    using namespace kraken::net;
    NativeObjectArchiveV2 input = archive_fixture();
    std::vector<Byte> encoded;
    CHECK(encode_native_object_archive_v2(input, encoded) ==
          NativeObjectArchiveErrorCode::None);
    CHECK(encoded.size() <= kNativeObjectArchiveMaxBytes);
    NativeObjectArchiveV2 decoded{};
    CHECK(decode_native_object_archive_v2(ByteView{encoded}, decoded) ==
          NativeObjectArchiveErrorCode::None);
    CHECK(decoded.digest != 0);
    CHECK(decoded.canonical_xml == input.canonical_xml);
    const std::string xml(reinterpret_cast<const char*>(decoded.canonical_xml.data()),
                          decoded.canonical_xml.size());
    CHECK(xml.find("Runtime") != std::string::npos);
    CHECK(xml.find("Visibility") != std::string::npos);
    CHECK(xml.find("ObjectId") == std::string::npos);
    CHECK(xml.find("AI") == std::string::npos);
    CHECK(decoded.manifest.size() == input.manifest.size());
    CHECK(decoded.manifest[0].archive_path == "Vehicle");
    CHECK(decoded.manifest[1].parent_path == "Vehicle");

    std::vector<Byte> second;
    CHECK(encode_native_object_archive_v2(decoded, second) ==
          NativeObjectArchiveErrorCode::None);
    CHECK(second == encoded);
}

void test_recursive_manifest_and_archive_bomb_rejection()
{
    using namespace kraken::net;
    NativeObjectArchiveV2 invalid = archive_fixture();
    std::vector<Byte> canonical_xml;
    CHECK(canonicalize_native_object_xml(ByteView{invalid.canonical_xml},
                                         canonical_xml) ==
          NativeObjectArchiveErrorCode::None);
    invalid.canonical_xml = canonical_xml;
    invalid.manifest[1].parent_path = "missing";
    CHECK(validate_native_object_archive_v2(invalid) ==
          NativeObjectArchiveErrorCode::InvalidManifest);
    invalid = archive_fixture();
    invalid.canonical_xml = canonical_xml;
    invalid.manifest.push_back(invalid.manifest[1]);
    CHECK(validate_native_object_archive_v2(invalid) ==
          NativeObjectArchiveErrorCode::InvalidManifest);

    std::vector<Byte> oversized(kNativeObjectArchiveMaxBytes + 1, Byte{0x41});
    CHECK(decode_native_object_archive_v2(ByteView{oversized}, invalid) ==
          NativeObjectArchiveErrorCode::InputTooLarge);

    std::string deep_xml;
    for (std::size_t depth = 0;
         depth != kNativeObjectArchiveMaxManifestDepth + 1; ++depth)
        deep_xml += "<Node>";
    deep_xml += "x";
    for (std::size_t depth = 0;
         depth != kNativeObjectArchiveMaxManifestDepth + 1; ++depth)
        deep_xml += "</Node>";
    std::vector<Byte> canonical;
    CHECK(canonicalize_native_object_xml(
              ByteView{reinterpret_cast<const Byte*>(deep_xml.data()),
                       deep_xml.size()}, canonical) ==
          NativeObjectArchiveErrorCode::InvalidXml);
}

void test_manifest_graph_and_runtime_policy_rejections()
{
    using namespace kraken::net;
    NativeObjectArchiveV2 invalid = archive_fixture();

    invalid.manifest.clear();
    CHECK(validate_native_object_archive_v2(invalid) ==
          NativeObjectArchiveErrorCode::InvalidManifest);

    invalid = archive_fixture();
    invalid.manifest[1].parent_path.clear();
    CHECK(validate_native_object_archive_v2(invalid) ==
          NativeObjectArchiveErrorCode::InvalidManifest);

    invalid = archive_fixture();
    invalid.manifest[1].resource_fingerprint = 0;
    CHECK(validate_native_object_archive_v2(invalid) ==
          NativeObjectArchiveErrorCode::InvalidResourceFingerprint);

    invalid = archive_fixture();
    invalid.manifest[1].archive_path = "Vehicle/Other";
    CHECK(validate_native_object_archive_v2(invalid) ==
          NativeObjectArchiveErrorCode::InvalidManifest);

    std::vector<Byte> canonical;
    const std::string policy_xml =
        "<Vehicle ObjectId=\"9\" AI=\"hostile\" Gameplay=\"gold\" "
        "Prototype=\"vehicle/prototype\"><Runtime><Visibility "
        "enabled=\"1\"/></Runtime></Vehicle>";
    CHECK(canonicalize_native_object_xml(
              ByteView{reinterpret_cast<const Byte*>(policy_xml.data()),
                       policy_xml.size()}, canonical) ==
          NativeObjectArchiveErrorCode::None);
    const std::string canonical_text(
        reinterpret_cast<const char*>(canonical.data()), canonical.size());
    CHECK(canonical_text.find("Visibility") != std::string::npos);
    CHECK(canonical_text.find("enabled=\"1\"") != std::string::npos);
    CHECK(canonical_text.find("ObjectId") == std::string::npos);
    CHECK(canonical_text.find("ObjId") == std::string::npos);
    CHECK(canonical_text.find("AI") == std::string::npos);
    CHECK(canonical_text.find("Gameplay") == std::string::npos);

    const std::string two_roots = "<Vehicle/><Vehicle/>";
    CHECK(canonicalize_native_object_xml(
              ByteView{reinterpret_cast<const Byte*>(two_roots.data()),
                       two_roots.size()}, canonical) ==
          NativeObjectArchiveErrorCode::InvalidXml);
}

void test_actual_vehicle_save_schema_is_sanitized_idempotently()
{
    using namespace kraken::net;
    // Representative structure copied from the real PlayerVehicle_0_0 save:
    // Object/Runtime/Wheels/WheelInfo/Wheel, Parts/<part>/Runtime/Geom, and
    // the repository/event branches emitted by Vehicle::SaveToXML.
    const std::string fixture = R"xml(
<Object ObjectId="35537" Name="PlayerVehicle_0_0" Belong="1100"
        Prototype="Bug01ForStart" Pos="864.199 255.886 937.255"
        Rot="0.003 -1.000 -0.009 0.008" AI="hostile" Gameplay="loot"
        LastDamageSource="2337">
  <Runtime LinearVelocity="-0.334 -0.017 -22.028" TargetId="-1"
            TimeAfterDeath="0.000" NumBlownParts="0" CurrentGear="2"
            Throttle="0.000" EngineRPM="4507.921" PathNum="-1"
            AIState="moving" QuestId="99">
    <EventRecipients><Event EventId="GE_OBJECT_DIE" Objects="2337"/></EventRecipients>
    <Wheels>
      <WheelInfo Id="0" present="1">
        <Wheel ObjectId="35527" Belong="1100" Prototype="bugWheel01"
               Pos="865.319 256.007 934.969"
               Rot="-0.008 0.451 -0.892 -0.001">
          <Runtime LinearVelocity="-0.341 -0.010 -22.030"
                   AngularVelocity="-35.114 -0.212 0.542" CurAngle="0.000"
                   Broken="1">
            <PhysicBody ModelName="bugWheel01"/>
          </Runtime>
        </Wheel>
      </WheelInfo>
    </Wheels>
  </Runtime>
  <Parts>
    <BASKET present="1" ObjectId="35535" Belong="1100"
            Prototype="bugCargo02_4x6">
      <Runtime AnimAction="3" ModelName="bugCargo02" CurAnimTime="315"
               MpHealth0="0.000" Health="600.000" Fuel="99.184">
        <Geom Position="0.006 2.333 -2.113"
              Rotation="0.0000 0.0000 0.0000 1.0000"/>
      </Runtime>
    </BASKET>
    <CABIN_SMALL_GUN present="1" ObjectId="35531" Belong="1100"
                     Prototype="pkt01">
      <Runtime ModelName="pkt01" TargetId="-1" ShellsInPool="400"
               IsFiring="0" WasShot="0"/>
    </CABIN_SMALL_GUN>
  </Parts>
  <Repository><Item ObjectId="68174" Prototype="ammo_chest_machinegun"/></Repository>
</Object>)xml";
    std::vector<Byte> canonical;
    CHECK(canonicalize_native_object_xml(
              ByteView{reinterpret_cast<const Byte*>(fixture.data()),
                       fixture.size()}, canonical) ==
          NativeObjectArchiveErrorCode::None);
    const std::string text(reinterpret_cast<const char*>(canonical.data()),
                           canonical.size());
    CHECK(text.find("<Wheels>") != std::string::npos);
    CHECK(text.find("<WheelInfo Id=\"0\" present=\"1\">") !=
          std::string::npos);
    CHECK(text.find("Prototype=\"bugWheel01\"") != std::string::npos);
    CHECK(text.find("Broken=\"1\"") != std::string::npos);
    CHECK(text.find("ModelName=\"bugCargo02\"") != std::string::npos);
    CHECK(text.find("<Geom Position=\"0.006 2.333 -2.113\"") !=
          std::string::npos);
    CHECK(text.find("Rotation=\"0.0000 0.0000 0.0000 1.0000\"") !=
          std::string::npos);
    CHECK(text.find("<Parts>") != std::string::npos);
    for (const char* forbidden : {
             "ObjectId", " Name=", "Belong=", "LinearVelocity",
             "AngularVelocity", "TargetId", "TimeAfterDeath",
             "NumBlownParts", "CurrentGear", "Throttle", "EngineRPM",
             "PathNum", "AIState", "QuestId", "LastDamageSource",
             "EventRecipients", "EventId", "PhysicBody", "AnimAction",
             "CurAnimTime", "MpHealth", "Health", "Fuel", "ShellsInPool",
             "IsFiring", "WasShot", "Repository"})
        CHECK(text.find(forbidden) == std::string::npos);

    std::vector<Byte> recanonicalized;
    CHECK(canonicalize_native_object_xml(ByteView{canonical}, recanonicalized) ==
          NativeObjectArchiveErrorCode::None);
    CHECK(recanonicalized == canonical);

    const std::string unknown_runtime =
        "<Object Prototype=\"Bug01ForStart\"><Runtime>"
        "<UnprovenRuntimeNode/></Runtime></Object>";
    CHECK(canonicalize_native_object_xml(
              ByteView{reinterpret_cast<const Byte*>(unknown_runtime.data()),
                       unknown_runtime.size()}, recanonicalized) ==
          NativeObjectArchiveErrorCode::InvalidXml);
}

void test_digest_and_chunk_transfer_replay_safety()
{
    using namespace kraken::net;
    NativeObjectArchiveV2 input = archive_fixture();
    const std::string filler(4'000, 'x');
    input.canonical_xml.insert(input.canonical_xml.end(),
                               reinterpret_cast<const Byte*>(filler.data()),
                               reinterpret_cast<const Byte*>(filler.data() + filler.size()));
    // Put the filler in a text node so the structural XML remains valid.
    input.canonical_xml.clear();
    const std::string xml = "<Vehicle Prototype=\"vehicle/prototype\"><Chassis "
                            "Resource=\"mesh/chassis\">" + filler +
                            "</Chassis><Runtime><Visibility enabled=\"1\"/>"
                            "</Runtime></Vehicle>";
    input.canonical_xml.assign(reinterpret_cast<const Byte*>(xml.data()),
                               reinterpret_cast<const Byte*>(xml.data() + xml.size()));
    std::vector<Byte> encoded;
    CHECK(encode_native_object_archive_v2(input, encoded) ==
          NativeObjectArchiveErrorCode::None);
    const NativeObjectArchiveDigest digest = native_object_archive_digest(encoded);
    std::vector<NativeObjectArchiveChunk> chunks;
    CHECK(make_native_object_archive_chunks(encoded, 77, 3, digest, chunks) ==
          NativeObjectArchiveTransferResult::Accepted);
    CHECK(chunks.size() > 2);

    NativeObjectArchiveReassembler reassembler;
    std::vector<Byte> complete;
    CHECK(reassembler.accept(chunks.front(), 100, complete) ==
          NativeObjectArchiveTransferResult::Accepted);
    CHECK(reassembler.accept(chunks.front(), 101, complete) ==
          NativeObjectArchiveTransferResult::Duplicate);
    for (std::size_t index = chunks.size(); index-- > 1;) {
        const auto result = reassembler.accept(chunks[index], 102 + index, complete);
        if (index != 1)
            CHECK(result == NativeObjectArchiveTransferResult::Accepted);
        else
            CHECK(result == NativeObjectArchiveTransferResult::Complete);
    }
    CHECK(complete == encoded);
    CHECK(reassembler.lookup(digest, complete));
    CHECK(complete == encoded);

    NativeObjectArchiveReassembler conflict;
    CHECK(conflict.accept(chunks[0], 1, complete) ==
          NativeObjectArchiveTransferResult::Accepted);
    NativeObjectArchiveChunk changed = chunks[0];
    changed.payload[0] = Byte{0x7f};
    CHECK(conflict.accept(changed, 2, complete) ==
          NativeObjectArchiveTransferResult::ConflictingChunk);

    NativeObjectArchiveReassembler bad_digest;
    NativeObjectArchiveChunk corrupted = chunks.front();
    corrupted.payload[0] = static_cast<Byte>(
        static_cast<std::uint8_t>(corrupted.payload[0]) ^ 0x01u);
    CHECK(bad_digest.accept(corrupted, 3, complete) ==
          NativeObjectArchiveTransferResult::Accepted);
    for (std::size_t index = 1; index != chunks.size(); ++index) {
        const auto result = bad_digest.accept(chunks[index], 4 + index,
                                              complete);
        if (index + 1 == chunks.size())
            CHECK(result == NativeObjectArchiveTransferResult::DigestMismatch);
        else
            CHECK(result == NativeObjectArchiveTransferResult::Accepted);
    }

    NativeObjectArchiveReassembler timeout;
    CHECK(timeout.accept(chunks[0], 10, complete) ==
          NativeObjectArchiveTransferResult::Accepted);
    CHECK(timeout.expire(10 + kNativeObjectArchiveTransferTimeoutMs + 1) == 1);
    CHECK(timeout.active_transfer_count() == 0);
}

void test_replica_archive_source_policy()
{
    const std::filesystem::path test_path(__FILE__);
    const std::vector<std::filesystem::path> candidates{
        test_path.parent_path().parent_path() / "source" / "net" /
            "native_object_archive.cpp",
        std::filesystem::current_path() / ".." / "source" / "net" /
            "native_object_archive.cpp",
        std::filesystem::current_path() / "source" / "net" /
            "native_object_archive.cpp"};
    std::string source;
    for (const auto& candidate : candidates) {
        std::ifstream input(candidate, std::ios::binary);
        if (input) {
            source.assign(std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>());
            break;
        }
    }
    CHECK(!source.empty());
    for (const char* forbidden : {"_EvaluateToDead", "_DeadActions",
                                  "BreakModel", "LoadRuntimeValues"})
        CHECK(source.find(forbidden) == std::string::npos);

    const std::size_t envelope = source.find(
        "CreateNode(hta::m3d::cmn::XML_NODE_ELEMENT, \"Object\")");
    const std::size_t save = source.find(
        "object.SaveToXML(xml_file, object_node)");
    const std::size_t attach = source.find(
        "xml_file->AddChild(object_node)");
    CHECK(envelope != std::string::npos);
    CHECK(save != std::string::npos);
    CHECK(attach != std::string::npos);
    CHECK(envelope < attach);
    CHECK(attach < save);
    CHECK(source.find(
              "CreateNode(hta::m3d::cmn::XML_NODE_DOCUMENT") ==
          std::string::npos);
    CHECK(source.find("if (!suspended_object.m_bNeedPostLoad)") !=
          std::string::npos);
    CHECK(source.find("if (vehicle->bIsUpdatingByODE())") ==
          std::string::npos);
    CHECK(source.find("object.SaveToXML(xml_file, document)") ==
          std::string::npos);
}

} // namespace

int main()
{
    test_bounded_input_validation();
    test_temp_path_validation();
    test_temp_file_raii();
    test_v2_round_trip_and_canonical_identity_exclusion();
    test_recursive_manifest_and_archive_bomb_rejection();
    test_manifest_graph_and_runtime_policy_rejections();
    test_actual_vehicle_save_schema_is_sanitized_idempotently();
    test_digest_and_chunk_transfer_replay_safety();
    test_replica_archive_source_policy();

    if (failures != 0)
        return 1;
    std::cout << "native object archive tests passed\n";
    return 0;
}
