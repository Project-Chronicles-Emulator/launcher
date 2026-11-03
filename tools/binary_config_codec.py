"""Bounded, lossless codec for the verified first BinaryConfig section map.

The remaining cache bytes stay opaque. An optional candidate-only IniBootstrap
splice uses the separately verified compact branch field order. New metadata
defaults remain assumptions until the original client accepts the archive.
The command writes candidates only under Emulator/out and never edits its input.
"""

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
from urllib.parse import urlsplit


PROJECT_ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SOURCE_SHA256 = "6b5f0126886393cde82e34031683675e8408531de3165087d1362a8e39f0f7e5"
ENGINE_MAP_OFFSET = 8
MAXIMUM_FILE_BYTES = 16 * 1024 * 1024
MAXIMUM_STRING_BYTES = 1024 * 1024
MAXIMUM_SECTIONS = 4096
MAXIMUM_PAIRS = 100000
MAXIMUM_ENGINE_OVERRIDES = 32
VERIFIED_OPTIONAL_TAIL_OFFSET = 0x1A27C0
VERIFIED_TRAILER_OFFSET = 0x1A2ECC
VERIFIED_OPTIONAL_PATH_COUNT = 38


@dataclass(frozen=True)
class FString:
    count: int
    raw: bytes

    @property
    def text(self):
        if self.count == 0:
            return ""
        width = 1 if self.count > 0 else 2
        encoding = "latin-1" if width == 1 else "utf-16le"
        return self.raw[:-width].decode(encoding)

    def encode(self):
        return struct.pack("<i", self.count) + self.raw

    @classmethod
    def create(cls, value, wide=False):
        if "\0" in value:
            raise ValueError("New FString values cannot contain embedded null characters")
        wide = wide or not value.isascii()
        if wide:
            raw = value.encode("utf-16le") + b"\0\0"
            if len(raw) > MAXIMUM_STRING_BYTES:
                raise ValueError("New FString exceeds the size limit")
            return cls(-len(raw) // 2, raw)
        raw = value.encode("ascii") + b"\0"
        if len(raw) > MAXIMUM_STRING_BYTES:
            raise ValueError("New FString exceeds the size limit")
        return cls(len(raw), raw)


class ArchiveReader:
    def __init__(self, data, offset=0):
        self.data = data
        self.position = offset

    def read_i32(self):
        if self.position + 4 > len(self.data):
            raise ValueError(f"Truncated integer at {self.position:#x}")
        value = struct.unpack_from("<i", self.data, self.position)[0]
        self.position += 4
        return value

    def read_count(self, maximum, label):
        value = self.read_i32()
        if not 0 <= value <= maximum:
            raise ValueError(f"Invalid {label} count at {self.position - 4:#x}")
        return value

    def read_string(self):
        count = self.read_i32()
        width = 1 if count >= 0 else 2
        byte_count = abs(count) * width
        if byte_count > MAXIMUM_STRING_BYTES or byte_count > len(self.data) - self.position:
            raise ValueError(f"String length exceeds bounds at {self.position - 4:#x}")
        raw = self.data[self.position:self.position + byte_count]
        self.position += byte_count
        if byte_count and raw[-width:] != bytes(width):
            raise ValueError(f"Missing string terminator at {self.position - byte_count:#x}")

        value = FString(count, raw)
        value.text
        return value

    def read_pairs(self):
        count = self.read_count(MAXIMUM_PAIRS, "pair")
        return [(self.read_string(), self.read_string()) for _ in range(count)]

    def read_sections(self):
        section_count = self.read_count(MAXIMUM_SECTIONS, "section")
        sections = []
        for _ in range(section_count):
            name = self.read_string()
            pairs = self.read_pairs()
            extra_pairs = self.read_pairs()
            sections.append(Section(name, pairs, extra_pairs))
        return sections


@dataclass
class Section:
    name: FString
    pairs: list
    extra_pairs: list

    def encode(self):
        return self.name.encode() + encode_pairs(self.pairs) + encode_pairs(self.extra_pairs)


@dataclass
class EngineMap:
    prefix: bytes
    sections: list
    suffix: bytes

    def encode_sections(self):
        encoded = bytearray(struct.pack("<i", len(self.sections)))
        for section in self.sections:
            encoded += section.encode()
        return bytes(encoded)

    def encode(self):
        return self.prefix + self.encode_sections() + self.suffix


def encode_pairs(pairs):
    encoded = bytearray(struct.pack("<i", len(pairs)))
    for key, value in pairs:
        encoded += key.encode()
        encoded += value.encode()
    return bytes(encoded)


def parse_engine_map(data):
    if not ENGINE_MAP_OFFSET + 4 <= len(data) <= MAXIMUM_FILE_BYTES:
        raise ValueError("BinaryConfig input size is outside the accepted range")

    reader = ArchiveReader(data, ENGINE_MAP_OFFSET)
    sections = reader.read_sections()
    return EngineMap(data[:ENGINE_MAP_OFFSET], sections, data[reader.position:])


def update_engine_value(engine, section_name, key_name, new_value):
    matching_sections = [section for section in engine.sections
                         if section.name.text == section_name]
    if len(matching_sections) > 1:
        raise ValueError("The target section is repeated; refusing an ambiguous update")
    if not matching_sections:
        section = Section(FString.create(section_name), [], [])
        engine.sections.append(section)
        matching_sections.append(section)

    section = matching_sections[0]
    matching_pairs = [(index, value) for index, (key, value) in enumerate(section.pairs)
                      if key.text == key_name]
    if len(matching_pairs) > 1:
        raise ValueError("The target key is repeated; refusing an ambiguous update")

    before = None
    if matching_pairs:
        index, old_value = matching_pairs[0]
        before = old_value.text
        key = section.pairs[index][0]
        section.pairs[index] = (key, FString.create(new_value, wide=old_value.count < 0))
    else:
        section.pairs.append((FString.create(key_name), FString.create(new_value)))

    return before


def ordered_records(
    engine, excluded_section=None, excluded_key=None, *, excluded_targets=None
):
    targets = set(excluded_targets or ())
    if excluded_section is not None and excluded_key is not None:
        targets.add((excluded_section, excluded_key))

    records = []
    for section in engine.sections:
        pairs = [(key.encode(), value.encode()) for key, value in section.pairs
                 if (section.name.text, key.text) not in targets]
        extras = [(key.encode(), value.encode()) for key, value in section.extra_pairs]
        is_target_section = any(name == section.name.text for name, _ in targets)
        if is_target_section and not pairs and not extras:
            continue
        records.append((section.name.encode(), pairs, extras))
    return records


def prepare_candidate(source, section_name, key_name, value, *, engine_overrides=()):
    if len(engine_overrides) > MAXIMUM_ENGINE_OVERRIDES:
        raise ValueError("Too many explicit Engine overrides")

    requested_changes = [(section_name, key_name, value)]
    for override in engine_overrides:
        if not isinstance(override, (tuple, list)) or len(override) != 3:
            raise ValueError("Each Engine override must contain a section, key, and string value")
        if not all(isinstance(item, str) for item in override):
            raise ValueError("Each Engine override must contain a section, key, and string value")
        if not override[0] or not override[1]:
            raise ValueError("Engine override section and key must be nonempty")
        requested_changes.append(tuple(override))

    targets = {(section, key) for section, key, _ in requested_changes}
    if len(targets) != len(requested_changes):
        raise ValueError("An Engine setting was supplied more than once")

    original = parse_engine_map(source)
    if original.encode() != source:
        raise ValueError("The unchanged first-map roundtrip did not preserve every byte")

    original_records = ordered_records(original, excluded_targets=targets)
    candidate = parse_engine_map(source)
    changes = []
    for section, key, new_value in requested_changes:
        old_value = update_engine_value(candidate, section, key, new_value)
        changes.append({
            "section": section,
            "key": key,
            "before": old_value,
            "after": new_value,
        })

    if ordered_records(candidate, excluded_targets=targets) != original_records:
        raise ValueError("A non-target section, ordered pair, encoding, or extra pair changed")

    encoded = candidate.encode()
    reparsed = parse_engine_map(encoded)
    if reparsed.encode() != encoded or reparsed.prefix != original.prefix:
        raise ValueError("Candidate failed its second lossless roundtrip")
    if reparsed.suffix != original.suffix:
        raise ValueError("The opaque cache suffix changed")

    evidence = {
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "candidate_sha256": hashlib.sha256(encoded).hexdigest(),
        "unchanged_roundtrip": True,
        "candidate_roundtrip": True,
        "non_target_ordered_records_identical": True,
        "prefix_bytes_identical": True,
        "opaque_suffix_bytes_identical": True,
        "opaque_suffix_sha256": hashlib.sha256(original.suffix).hexdigest(),
        "opaque_suffix_bytes": len(original.suffix),
        "source_engine_map_start": ENGINE_MAP_OFFSET,
        "source_engine_map_end": len(source) - len(original.suffix),
        "candidate_engine_map_end": len(encoded) - len(reparsed.suffix),
        "section_count_before": len(original.sections),
        "section_count_after": len(reparsed.sections),
        "ordered_pair_count_before": sum(len(section.pairs) for section in original.sections),
        "ordered_pair_count_after": sum(len(section.pairs) for section in reparsed.sections),
        "change": changes[0],
        "engine_overrides": changes[1:],
        "scope": (
            "First Engine map only; opaque cache suffix retained. Not an IniBootstrap archive."
        ),
    }
    return encoded, evidence


def output_path(value):
    requested = Path(value)
    if requested.is_absolute():
        candidate = requested.resolve()
    else:
        candidate = (PROJECT_ROOT / requested).resolve()
    allowed_root = (PROJECT_ROOT / "out").resolve()
    if not allowed_root.is_relative_to(PROJECT_ROOT) or not candidate.is_relative_to(allowed_root):
        raise ValueError("Candidate files must remain inside Emulator/out/")
    return candidate


def read_dynamic_ini(path):
    """Read the simple ordered syntax present in this extracted custom INI.

    This intentionally rejects unimplemented operators and multiline strings.
    It is not a general UE config hierarchy evaluator.
    """
    if path.stat().st_size > MAXIMUM_STRING_BYTES:
        raise ValueError("Dynamic INI exceeds the candidate input size limit")
    sections = []
    section = None
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = Section(FString.create(line[1:-1]), [], [])
            sections.append(section)
            continue
        if section is None or "=" not in line or line.endswith("\\"):
            raise ValueError(f"Unsupported dynamic INI syntax on line {line_number}")

        key, value = (part.strip() for part in line.split("=", 1))
        if not key or key[0] in "+.-@":
            raise ValueError(f"Unsupported dynamic INI operator on line {line_number}")
        if key.startswith("!"):
            removed_key = key[1:]
            if not removed_key or value.upper() != "CLEAR_ARRAY":
                raise ValueError(f"Unsupported clear operation on line {line_number}")
            section.pairs = [
                (existing_key, existing_value)
                for existing_key, existing_value in section.pairs
                if existing_key.text.casefold() != removed_key.casefold()
            ]
            continue
        if value.startswith('"'):
            if len(value) < 2 or not value.endswith('"') or "\\" in value:
                raise ValueError(f"Unsupported quoted string on line {line_number}")
            value = value[1:-1]
        section.pairs.append((FString.create(key), FString.create(value)))

    if not sections:
        raise ValueError("Dynamic INI contains no sections")
    return sections


def encode_config_state(sections):
    # Metadata defaults below are explicit candidate assumptions. The section map,
    # three bool32 values, FName-as-FString, FString and empty-map order is verified.
    section_bytes = EngineMap(b"", sections, b"").encode_sections()
    metadata = struct.pack("<iii", 0, 0, 0)
    metadata += FString.create("None").encode()
    metadata += FString(0, b"").encode()
    metadata += struct.pack("<i", 0)
    return section_bytes + metadata


def native_bootstrap_request_url(base_url):
    # The client concatenates this suffix rather than resolving a relative URI.
    request_url = base_url + "api/auth/BootstrapReq"
    parsed = urlsplit(request_url)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise ValueError("Native bootstrap base must be an absolute HTTP(S) URL")
    root_request = parsed.path == "/api/auth/BootstrapReq"
    grant_request = re.fullmatch(
        r"/launch/[0-9a-f]{64}/api/auth/BootstrapReq", parsed.path
    ) is not None
    if not (root_request or grant_request) or parsed.query or parsed.fragment:
        raise ValueError("Native bootstrap base must be a root or launch-grant URL ending in /")
    return request_url


def read_candidate_config_state(reader):
    sections = reader.read_sections()
    flags = tuple(reader.read_count(1, "config flag") for _ in range(3))
    state_name = reader.read_string()
    state_file = reader.read_string()
    if reader.read_i32() != 0:
        raise ValueError("Candidate config-state metadata map must remain empty")
    return sections, flags, state_name, state_file


def split_binary_config_trailer(data, trailer_offset):
    """Separate a typed trailer only at an independently established boundary.

    BinaryConfig appends this map after the shared cache serializer. IniBootstrap
    instead reads its fourteen global strings immediately at that boundary.
    A successful map parse alone does not establish where the shared body ends.
    """
    if not 4 <= trailer_offset <= len(data) - 4:
        raise ValueError("BinaryConfig trailer offset is outside the input")
    reader = ArchiveReader(data, trailer_offset)
    entry_count = reader.read_count(MAXIMUM_PAIRS, "BinaryConfig trailer entry")
    entries = []
    for _ in range(entry_count):
        key = reader.read_string()
        payload_bytes = reader.read_count(MAXIMUM_FILE_BYTES, "BinaryConfig trailer payload")
        if payload_bytes > len(data) - reader.position:
            raise ValueError("Truncated BinaryConfig trailer payload")
        payload = data[reader.position:reader.position + payload_bytes]
        reader.position += payload_bytes
        entries.append({
            "key": key.text,
            "payload_bytes": payload_bytes,
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
        })
    if reader.position != len(data):
        raise ValueError("BinaryConfig trailer does not consume the exact input end")
    return data[:trailer_offset], {
        "offset": trailer_offset,
        "bytes": len(data) - trailer_offset,
        "entry_count": entry_count,
        "entries": entries,
        "sha256": hashlib.sha256(data[trailer_offset:]).hexdigest(),
    }


def strip_verified_binary_config_trailer(source, patched_cache):
    """Prove the known build's shared-body endpoint before removing its trailer."""
    if hashlib.sha256(source).hexdigest() != EXPECTED_SOURCE_SHA256:
        raise ValueError("Shared-body boundary is established only for the verified source build")
    reader = ArchiveReader(source, VERIFIED_OPTIONAL_TAIL_OFFSET)
    if reader.read_count(1, "optional tail present") != 1:
        raise ValueError("Verified optional shared-cache tail is absent")
    path_count = reader.read_count(MAXIMUM_PAIRS, "optional tail path")
    if path_count != VERIFIED_OPTIONAL_PATH_COUNT:
        raise ValueError("Verified optional shared-cache path count changed")
    paths = [reader.read_string().text for _ in range(path_count)]
    if reader.position != VERIFIED_TRAILER_OFFSET:
        raise ValueError("Optional shared-cache tail ends at an unexpected boundary")
    if paths[0] != "../../../Engine/Config/Windows/WindowsGame.ini":
        raise ValueError("First verified optional path changed")
    if paths[-1] != "../../../X5/Config/Windows/WindowsScalability.ini":
        raise ValueError("Last verified optional path changed")
    _, original_trailer = split_binary_config_trailer(source, reader.position)
    if original_trailer["entry_count"] != 0 or original_trailer["bytes"] != 4:
        raise ValueError("Verified BinaryConfig trailer must be precisely an empty four-byte map")

    offset_delta = len(patched_cache) - len(source)
    patched_tail_offset = VERIFIED_OPTIONAL_TAIL_OFFSET + offset_delta
    if patched_cache[patched_tail_offset:] != source[VERIFIED_OPTIONAL_TAIL_OFFSET:]:
        raise ValueError("The verified shared-cache tail changed during the Engine patch")
    shared_body, patched_trailer = split_binary_config_trailer(
        patched_cache, VERIFIED_TRAILER_OFFSET + offset_delta
    )
    patched_trailer["source_offset"] = VERIFIED_TRAILER_OFFSET
    patched_trailer["optional_tail_source_offset"] = VERIFIED_OPTIONAL_TAIL_OFFSET
    patched_trailer["optional_path_count"] = path_count
    patched_trailer["optional_tail_identical"] = True
    patched_trailer["boundary_evidence"] = (
        "Shared 141971430 optional 14194D370/141943570 reads count and FString set; "
        "BinaryConfig-only 14194C5B0/141942540 reads the following empty byte-array map"
    )
    return shared_body, patched_trailer


def splice_dynamic_candidate(shared_cache, sections, filename, global_ini_names):
    if shared_cache[:4] != bytes(4):
        raise ValueError("The verified source must have an empty initial dynamic-file map")
    if not isinstance(global_ini_names, list) or len(global_ini_names) != 14:
        raise ValueError("IniBootstrap requires exactly fourteen supplied global INI strings")
    if not all(isinstance(name, str) for name in global_ini_names):
        raise ValueError("IniBootstrap requires exactly fourteen supplied global INI strings")
    if not filename or "\0" in filename:
        raise ValueError("Supply the exact nonempty dynamic filename key")

    active_state = encode_config_state(sections)
    empty_state = encode_config_state([])
    branch = struct.pack("<i", 0) + active_state
    branch += struct.pack("<ii", 0, 0)
    branch += empty_state + empty_state
    branch += FString.create("None").encode() + FString.create(filename).encode()
    injected_prefix = struct.pack("<i", 1) + FString.create(filename).encode() + branch
    trailing_names = b"".join(
        FString.create(name).encode() if name else FString(0, b"").encode()
        for name in global_ini_names
    )
    candidate = injected_prefix + shared_cache[4:] + trailing_names
    if len(candidate) > MAXIMUM_FILE_BYTES:
        raise ValueError("Assembled candidate exceeds the file size limit")

    # Independently walk the inserted branch and appended strings. The original
    # core-cache payload is compared byte-for-byte and is never reconstructed.
    reader = ArchiveReader(candidate)
    if reader.read_i32() != 1 or reader.read_string().text != filename:
        raise ValueError("Candidate dynamic map header did not roundtrip")
    if reader.read_i32() != 0:
        raise ValueError("Unexpected candidate branch flag")
    decoded_sections, _, _, _ = read_candidate_config_state(reader)
    decoded_active = EngineMap(b"", decoded_sections, b"").encode_sections()
    original_active = EngineMap(b"", sections, b"").encode_sections()
    if decoded_active != original_active:
        raise ValueError("Candidate active configuration did not roundtrip")
    if reader.read_i32() != 0 or reader.read_i32() != 0:
        raise ValueError("Unexpected candidate branch map or integer")
    for _ in range(2):
        empty_sections, _, _, _ = read_candidate_config_state(reader)
        if empty_sections:
            raise ValueError("Candidate inactive states must remain empty")
    if reader.read_string().text != "None" or reader.read_string().text != filename:
        raise ValueError("Candidate branch metadata did not roundtrip")
    if reader.position != len(injected_prefix):
        raise ValueError("Candidate dynamic branch boundary differs")

    shared_end = reader.position + len(shared_cache) - 4
    if candidate[reader.position:shared_end] != shared_cache[4:]:
        raise ValueError("Original shared-cache payload changed during splice")
    reader.position = shared_end
    decoded_names = [reader.read_string().text for _ in range(14)]
    if decoded_names != global_ini_names or reader.position != len(candidate):
        raise ValueError("Candidate trailing INI names did not roundtrip")

    evidence = {
        "candidate_only": True,
        "dynamic_filename": filename,
        "dynamic_section_count": len(sections),
        "injected_prefix_bytes": len(injected_prefix),
        "shared_core_payload_bytes": len(shared_cache) - 4,
        "shared_core_payload_identical": True,
        "shared_core_payload_sha256": hashlib.sha256(shared_cache[4:]).hexdigest(),
        "trailing_ini_names": global_ini_names,
        "inserted_branch_and_trailing_names_roundtrip": True,
        "assumptions": [
            "New branch flag and all three flags in each new config state are zero",
            "New generic metadata maps and integer are empty or zero",
            "Unused FName metadata is None; state filename metadata is empty",
            "Branch filename metadata repeats the explicitly supplied dynamic map key",
            "Fourteen global INI values are caller-supplied; "
            "their provenance requires separate review",
        ],
        "scope": (
            "Opaque shared-core splice using the verified compact archive field order; "
            "runtime acceptance unproven"
        ),
    }
    return candidate, evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path,
                        default=PROJECT_ROOT / "out/native-config/raw/X5/Config/BinaryConfig.ini")
    parser.add_argument("--output", required=True, help="New candidate file under Emulator/out/")
    parser.add_argument("--section", default="SSL")
    parser.add_argument("--key", default="DebuggingCertificatePath")
    parser.add_argument("--value", required=True)
    parser.add_argument(
        "--engine-override",
        action="append",
        nargs=3,
        metavar=("SECTION", "KEY", "VALUE"),
        default=[],
        help="Overlay one explicitly supplied Engine setting; repeat for additional settings",
    )
    parser.add_argument("--dynamic-ini", type=Path)
    parser.add_argument("--dynamic-filename")
    parser.add_argument("--global-ini-names", type=Path)
    parser.add_argument(
        "--bootstrap-url", help="Native base URL, for example http://127.0.0.1:8080/"
    )
    parser.add_argument(
        "--redact-bootstrap-url", action="store_true",
        help="Omit a launch grant URL from the evidence sidecar",
    )
    arguments = parser.parse_args()

    source_path = arguments.input.resolve()
    if source_path.stat().st_size > MAXIMUM_FILE_BYTES:
        raise ValueError("Source file exceeds the parser size limit")
    source = source_path.read_bytes()
    if hashlib.sha256(source).hexdigest() != EXPECTED_SOURCE_SHA256:
        raise ValueError(
            "Input is not the verified demo BinaryConfig; revalidate a new build first"
        )

    destination = output_path(arguments.output)
    report_path = destination.with_suffix(destination.suffix + ".evidence.json")
    if source_path in (destination, report_path) or destination.exists() or report_path.exists():
        raise ValueError("Input and existing candidate/report files are never overwritten")

    candidate, evidence = prepare_candidate(
        source,
        arguments.section,
        arguments.key,
        arguments.value,
        engine_overrides=arguments.engine_override,
    )
    dynamic_options = [arguments.dynamic_ini, arguments.dynamic_filename,
                       arguments.global_ini_names, arguments.bootstrap_url]
    if any(option is not None for option in dynamic_options):
        if not all(option is not None for option in dynamic_options):
            raise ValueError(
                "All four dynamic/IniBootstrap candidate options must be supplied together"
            )
        request_url = native_bootstrap_request_url(arguments.bootstrap_url)
        sections = read_dynamic_ini(arguments.dynamic_ini)
        dynamic_state = EngineMap(b"", sections, b"")
        original_records = ordered_records(dynamic_state, "Server", "BootstrapUrl")
        update_engine_value(dynamic_state, "Server", "BootstrapUrl", arguments.bootstrap_url)
        if ordered_records(dynamic_state, "Server", "BootstrapUrl") != original_records:
            raise ValueError("A non-bootstrap dynamic INI record changed")
        if arguments.global_ini_names.stat().st_size > MAXIMUM_STRING_BYTES:
            raise ValueError("Global INI names input exceeds the size limit")
        names = json.loads(arguments.global_ini_names.read_text(encoding="utf-8-sig"))
        candidate, trailer_evidence = strip_verified_binary_config_trailer(source, candidate)
        candidate, splice_evidence = splice_dynamic_candidate(
            candidate, sections, arguments.dynamic_filename, names)
        evidence["ini_bootstrap_candidate"] = splice_evidence
        evidence["removed_binary_config_only_trailer"] = trailer_evidence
        evidence["dynamic_ini_source_sha256"] = hashlib.sha256(
            arguments.dynamic_ini.read_bytes()).hexdigest()
        evidence["dynamic_non_bootstrap_records_identical"] = True
        if arguments.redact_bootstrap_url:
            evidence["native_bootstrap_request_url"] = "[redacted]"
        else:
            evidence["native_bootstrap_request_url"] = request_url
        evidence["candidate_sha256"] = hashlib.sha256(candidate).hexdigest()
        evidence["scope"] = (
            "Candidate IniBootstrap with SSL addition and custom dynamic branch; "
            "not installed or runtime-verified"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("xb") as output:
        output.write(candidate)
    with report_path.open("x", encoding="utf-8") as output:
        json.dump(evidence, output, indent=2)
        output.write("\n")
    print(json.dumps({"candidate": str(destination), "evidence": str(report_path),
                      "unchanged_roundtrip": True, "non_target_records_identical": True,
                      "opaque_suffix_identical": True}))


if __name__ == "__main__":
    main()
