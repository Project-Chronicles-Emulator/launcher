"""Publish an immutable, Ed25519-signed client release for chronicles-server.

The source must be a curated distribution tree, not a live player profile. Private keys and
bootstrap source inputs remain outside the served directory. Only the channel pointer changes.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import urlsplit


MAXIMUM_FILE_BYTES = 64 * 1024**3
MAXIMUM_MANIFEST_BYTES = 8 * 1024**2
PUBLIC_KEY_DER_PREFIX = bytes.fromhex("302a300506032b6570032100")


def disallowed_game_runtime(relative_path):
    path = relative_path.lower()
    if not path.startswith("x5/binaries/win64/"):
        return False
    name = path.removeprefix("x5/binaries/win64/")
    return bool(re.fullmatch(r"(?:concrt140|msvcp140(?:_[a-z0-9_]+)?|vccorlib140|vcruntime140(?:_[a-z0-9_]+)?)\.dll", name))


def service_origin(value):
    value = value.removesuffix("/")
    parsed = urlsplit(value)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise ValueError("An absolute HTTP(S) API origin is required")
    if parsed.path or parsed.query or parsed.fragment or parsed.username or parsed.password:
        raise ValueError("API origin must not contain a path, query, fragment or userinfo")
    if parsed.scheme == "http" and parsed.hostname not in {"127.0.0.1", "localhost", "::1"}:
        raise ValueError("Remote API origins require HTTPS")
    if parsed.port is not None and not 1 <= parsed.port <= 65535:
        raise ValueError("Invalid API port")
    return value


def safe_path(value):
    if not value or len(value) > 220 or value.startswith("/") or value.endswith("/"):
        return False
    for segment in value.split("/"):
        if not re.fullmatch(r"[A-Za-z0-9_. -]+", segment):
            return False
        if segment in {".", ".."} or segment.endswith((".", " ")):
            return False
        if segment.lower().startswith(".chronicles"):
            return False
        device = segment.split(".", 1)[0].lower()
        if device in {"con", "prn", "aux", "nul"} or re.fullmatch(r"(?:com|lpt)[0-9]", device):
            return False
    return True


def public_key_hex(openssl, private_key):
    result = subprocess.run(
        [openssl, "pkey", "-in", str(private_key), "-pubout", "-outform", "DER"],
        check=True,
        capture_output=True,
    ).stdout
    if not result.startswith(PUBLIC_KEY_DER_PREFIX) or len(result) != len(PUBLIC_KEY_DER_PREFIX) + 32:
        raise ValueError("Release signing requires an Ed25519 private key")
    return result[-32:].hex()


def publish_blob(source, blob_directory):
    if source.is_symlink() or not source.is_file() or source.stat().st_size > MAXIMUM_FILE_BYTES:
        raise ValueError(f"Unsupported release source: {source}")
    digest = hashlib.sha256()
    size = 0
    with tempfile.NamedTemporaryFile(dir=blob_directory, prefix=".publishing-", delete=False) as output:
        temporary = Path(output.name)
        try:
            with source.open("rb") as input_file:
                for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
                    size += len(chunk)
                    if size > MAXIMUM_FILE_BYTES:
                        raise ValueError("Release source grew beyond the file-size limit")
                    digest.update(chunk)
                    output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        except BaseException:
            output.close()
            temporary.unlink(missing_ok=True)
            raise
    fingerprint = digest.hexdigest()
    destination = blob_directory / fingerprint
    if destination.exists():
        with destination.open("rb") as existing:
            if hashlib.file_digest(existing, "sha256").hexdigest() != fingerprint:
                temporary.unlink()
                raise ValueError("An existing immutable blob was changed")
        temporary.unlink()
    else:
        temporary.replace(destination)
    return {"sha256": fingerprint, "size": size}


def publish(arguments):
    source = arguments.source.resolve(strict=True)
    destination = arguments.output.resolve()
    key = arguments.private_key.resolve(strict=True)
    api_origin = service_origin(arguments.api_origin)
    if not re.fullmatch(r"[a-z0-9-]{1,32}", arguments.channel):
        raise ValueError("Invalid patch channel")
    if not 1 <= arguments.sequence <= 2**63 - 1 or not 1 <= len(arguments.version) <= 64:
        raise ValueError("A positive release sequence and a 1–64 character version are required")
    if source == destination or destination.is_relative_to(source) or source.is_relative_to(destination):
        raise ValueError("Release input and published output must be separate trees")
    if key.is_relative_to(source) or key.is_relative_to(destination):
        raise ValueError("The signing private key must stay outside release input/output")
    public_key = public_key_hex(arguments.openssl, key)
    channels = destination / "channels"
    blobs = destination / "blobs"
    channels.mkdir(parents=True, exist_ok=True)
    blobs.mkdir(parents=True, exist_ok=True)
    lock_path = destination / ".publish.lock"
    lock = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        channel_path = channels / f"{arguments.channel}.json"
        if channel_path.exists():
            previous = json.loads(channel_path.read_text(encoding="utf-8"))
            previous_manifest = json.loads(previous["payload"])
            if arguments.sequence <= previous_manifest["sequence"]:
                raise ValueError("Release sequences must increase; channel rollback is forbidden")
        files = []
        names = set()
        for path in sorted(source.rglob("*")):
            if path.is_symlink() or path.is_junction():
                raise ValueError("Release sources must not contain symlinks or junctions")
            if path.is_dir():
                continue
            relative = path.relative_to(source).as_posix()
            if disallowed_game_runtime(relative):
                raise ValueError(f"Install the system Visual C++ runtime instead of packaging {relative}")
            if not safe_path(relative) or relative.lower() in names:
                raise ValueError(f"Unsafe or duplicate release path: {relative}")
            if path.name.lower() == ".env" or path.suffix.lower() in {".key", ".pfx", ".p12"}:
                raise ValueError(f"Credential-like files do not belong in a client release: {relative}")
            names.add(relative.lower())
            files.append({"path": relative, **publish_blob(path, blobs)})
        if not files or len(files) > 100000 or sum(entry["size"] for entry in files) > 1024**4:
            raise ValueError("Empty or oversized client release")
        manifest = {
            "application": "chronicles",
            "format": 1,
            "channel": arguments.channel,
            "version": arguments.version,
            "sequence": arguments.sequence,
            "api_origin": api_origin,
            "files": files,
        }
        if arguments.bootstrap_template:
            template = arguments.bootstrap_template.resolve(strict=True)
            if template.stat().st_size > 16 * 1024**2:
                raise ValueError("Bootstrap template exceeds its size limit")
            placeholder = (api_origin + "/launch/" + "0" * 64 + "/").encode("ascii")
            template_bytes = template.read_bytes()
            offset = template_bytes.find(placeholder)
            if template_bytes.count(placeholder) != 1 or offset < 4:
                raise ValueError("Bootstrap template must contain exactly one grant placeholder")
            expected = (len(placeholder) + 1).to_bytes(4, "little")
            if template_bytes[offset - 4:offset] != expected:
                raise ValueError("Bootstrap placeholder must be an ASCII FString")
            manifest["bootstrap"] = publish_blob(template, blobs)
        payload = json.dumps(manifest, ensure_ascii=True, separators=(",", ":"))
        with tempfile.TemporaryDirectory(prefix="chronicles-sign-") as signing_directory:
            payload_path = Path(signing_directory) / "payload.json"
            signature_path = Path(signing_directory) / "signature.bin"
            payload_path.write_bytes(payload.encode("utf-8"))
            subprocess.run(
                [arguments.openssl, "pkeyutl", "-sign", "-rawin", "-inkey", str(key),
                 "-in", str(payload_path), "-out", str(signature_path)],
                check=True,
                capture_output=True,
            )
            signature = signature_path.read_bytes()
        if len(signature) != 64:
            raise ValueError("Invalid Ed25519 signature length")
        envelope = json.dumps({"payload": payload, "signature": signature.hex()}, indent=2)
        if len(envelope.encode("utf-8")) > MAXIMUM_MANIFEST_BYTES:
            raise ValueError("Signed manifest exceeds its size limit")
        temporary_channel = channel_path.with_suffix(".tmp")
        with temporary_channel.open("w", encoding="utf-8", newline="\n") as output:
            output.write(envelope + "\n")
            output.flush()
            os.fsync(output.fileno())
        temporary_channel.replace(channel_path)
        profile = {
            "server_url": api_origin,
            "patch_public_key": public_key,
            "patch_channel": arguments.channel,
        }
        if not getattr(arguments, "no_profile", False):
            (destination / "launcher-config.json").write_text(
                json.dumps(profile, indent=2) + "\n", encoding="utf-8"
            )
        return {"channel": arguments.channel, "version": arguments.version,
                "sequence": arguments.sequence, "files": len(files), "public_key": public_key,
                "manifest": str(channel_path)}
    finally:
        os.close(lock)
        lock_path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--private-key", type=Path, required=True)
    parser.add_argument("--api-origin", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--sequence", type=int, required=True)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--bootstrap-template", type=Path)
    parser.add_argument("--no-profile", action="store_true",
                        help="Do not replace the existing generated game launcher profile")
    parser.add_argument("--openssl", default="openssl")
    print(json.dumps(publish(parser.parse_args()), indent=2))


if __name__ == "__main__":
    main()
