"""Prepare launcher inputs from your own extraction, without modifying the client."""

import argparse
import hashlib
import json
import os
from pathlib import Path

import binary_config_codec as codec


def copy_input(source, destination):
    data = source.read_bytes()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if destination.read_bytes() != data:
            raise ValueError(f"Existing input differs; choose a new runtime: {destination}")
        return

    with destination.open("xb") as output:
        output.write(data)


def prepare(args):
    runtime = args.runtime.resolve()
    client = args.client.resolve()
    if runtime == client or runtime.is_relative_to(client):
        raise ValueError("Launcher runtime must be outside the client installation")
    if not (client / "X5/Binaries/Win64/ArcheAgeChronicles.exe").is_file():
        raise ValueError("The selected client lacks the expected executable")
    if args.certificate.stat().st_size > 65536:
        raise ValueError("Public certificate exceeds the supported PEM size")
    certificate = args.certificate.read_bytes()
    if b"BEGIN CERTIFICATE" not in certificate or b"PRIVATE KEY" in certificate:
        raise ValueError("Supply the world's public PEM certificate without a private key")

    config = args.extraction.resolve() / "raw/X5/Config"
    binary = config / "BinaryConfig.ini"
    dynamic = config / "DefaultX5Game.ini"
    if binary.stat().st_size > codec.MAXIMUM_FILE_BYTES:
        raise ValueError("BinaryConfig exceeds the supported input limit")
    if hashlib.sha256(binary.read_bytes()).hexdigest() != codec.EXPECTED_SOURCE_SHA256:
        raise ValueError("BinaryConfig does not match the researched build")
    codec.read_dynamic_ini(dynamic)

    destination = runtime / "out/native-config"
    for source in [binary, dynamic]:
        copy_input(source, destination / "raw/X5/Config" / source.name)
    copy_input(args.certificate, runtime / "out/native-server/localhost.pem")

    # This is the researched cache's 14-entry layout. Paths belong to this user,
    # rather than the machine where the original research was performed.
    config_directory = Path(os.environ["LOCALAPPDATA"]).as_posix() + "/X5/Saved/Config/Windows/"
    names = [
        "", "", "", "", "", config_directory + "Compat.ini",
        config_directory + "Lightmass.ini", "Scalability", "Hardware", "Input",
        "Game", "GameUserSettings", "RuntimeOptions", "Engine",
    ]
    names_path = destination / "ini-bootstrap-names.candidate.json"
    text = json.dumps(names, indent=2) + "\n"
    if names_path.exists() and names_path.read_text(encoding="utf-8") != text:
        raise ValueError("Existing INI metadata differs; choose a new runtime")
    names_path.write_text(text, encoding="utf-8")

    tools = Path(__file__).resolve().parent
    for relative in [
        "binary_config_codec.py", "launch_private_client.ps1",
        "private_client_firewall.ps1", "xigncode_shim_experiment/install_private_shim.ps1",
    ]:
        copy_input(tools / relative, runtime / "tools" / relative)

    settings_path = runtime / "out/launcher/settings.json"
    settings_path.parent.mkdir(parents=True, exist_ok=True)
    settings = json.loads(settings_path.read_text(encoding="utf-8")) if settings_path.exists() else {}
    settings.update(
        game_directory=str(client), game_directory_explicit=True,
        client_launch_target=args.client_mode, backend_mode="usermode", auto_start_server=False,
    )
    settings_path.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
    print(f"Local inputs prepared under {runtime}")
    print("No client files were modified. Keep this runtime and its extracted inputs private.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extraction", type=Path, required=True,
                        help="Your snapshot containing raw/X5/Config")
    parser.add_argument("--certificate", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--runtime", type=Path,
                        default=Path(__file__).resolve().parents[1] / "runtime")
    parser.add_argument("--client-mode", choices=["steam", "private_copy"], default="steam",
                        help="private_copy requires an already prepared compatible private client")
    args = parser.parse_args()
    try:
        prepare(args)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Local preparation failed: {error}\n")


if __name__ == "__main__":
    main()
