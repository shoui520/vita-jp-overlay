"""Exercise the real package installer with a local filesystem FTP substitute."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ReleaseInstall(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / "package"
        self.device = self.root / "device"
        for folder in ("tools", "ur0/tai", "ur0/data/VitaJPOverlay",
                       "ux0/data/VitaJPOverlay/ocr", "ux0/data/VitaJPOverlay/dictionaries"):
            (self.package / folder).mkdir(parents=True, exist_ok=True)
        for name in ("install_ftp.sh", "package_release.py", "prepare_ocr.py", "prepare_plaintext.py",
                     "migrate_config.py", "patch_tai_config.py", "collect_vita.py", "analyse_selftest.py",
                     "model_tiers.json"):
            shutil.copyfile(ROOT / "tools" / name, self.package / "tools" / name)
        shutil.copyfile(ROOT / "tools/install-vita.sh", self.package / "install-vita.sh")
        for name in ("config.ini", "ux0/data/VitaJPOverlay/config.ini"):
            shutil.copyfile(ROOT / "docs/config.example.ini", self.package / name)
        for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                     "ur0/data/VitaJPOverlay/vitajpoverlay.rco", "JPOverlaySettings.vpk",
                     "ur0/tai/VitaJPOverlay_OCR.suprx",
                     "ux0/data/VitaJPOverlay/ocr/FL10_w8.vocr", "ux0/data/VitaJPOverlay/ocr/H15_w8.vocr",
                     "ux0/data/VitaJPOverlay/ocr/VOCR3_H15_w8.vocr", "ux0/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr",
                     "ux0/data/VitaJPOverlay/ocr/vocr_lm_o3_4000k.vlm", "ux0/data/VitaJPOverlay/ocr/LM-ATTRIBUTION.md",
                     "ux0/data/VitaJPOverlay/ocr/VOCR4_W20_w8.vocr",
                     "ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict"):
            (self.package / name).write_bytes(("test payload " + name).encode())
        files = {str(p.relative_to(self.package)): {"bytes": p.stat().st_size,
                 "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                 for p in self.package.rglob("*") if p.is_file()}
        (self.package / "package-manifest.json").write_text(json.dumps({"files": files}))
        self.original_tai = b"*KERNEL\r\nur0:tai/Other.skprx\r\n*main\r\n*ALL\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n"
        for partition in ("ur0", "ux0"):
            (self.device / partition / "tai").mkdir(parents=True)
            (self.device / partition / "tai/config.txt").write_bytes(self.original_tai)
        self.data = self.device / "ux0/data/VitaJPOverlay"
        self.data.mkdir(parents=True)
        (self.data / "config.ini").write_text("dictionary = hachidori\nocr_backend = lens\nfont_size_ja = 24\njpdb_api_key = test-key\n")
        (self.data / "games.ini").write_text("keep existing choices")
        (self.data / "region.ini").write_text("keep existing region")
        binary = self.root / "bin"
        binary.mkdir()
        curl = binary / "curl"
        curl.write_text("""#!/usr/bin/env python3
import os, pathlib, shutil, sys
from urllib.parse import urlparse
a = sys.argv[1:]
p = urlparse(a[-1]).path.lstrip('/').replace(':', '')
remote = pathlib.Path(os.environ['FAKE_DEVICE']) / p
if '-T' in a:
    remote.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(a[a.index('-T') + 1], remote)
elif '-o' in a:
    if not remote.is_file(): sys.exit(22)
    out = a[a.index('-o') + 1]
    if out != '/dev/null':
        shutil.copyfile(remote, out)
        if os.environ.get('CORRUPT_READBACK') == p:
            pathlib.Path(out).write_bytes(b'corrupted transfer')
""")
        curl.chmod(0o755)
        self.environment = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ["PATH"],
                                FAKE_DEVICE=str(self.device))
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(10)
        self.addCleanup(self.listener.close)

    def install(self, script="install-vita.sh", *options):
        return subprocess.run(["bash", str(self.package / script), *options,
                               "127.0.0.1:" + str(self.listener.getsockname()[1])],
                              env=self.environment, capture_output=True, text=True, timeout=30)

    def tai(self, partition):
        return (self.device / partition / "tai/config.txt").read_bytes()

    def assert_unchanged(self, result):
        """Nothing reached the Vita: no plugin, backup, boot record or setting."""
        self.assertFalse((self.device / "ur0/tai/VitaJPOverlay_Kernel.skprx").exists(), result.stdout)
        self.assertFalse((self.device / "ur0/data/VitaJPOverlay/boot.txt").exists())
        for partition in ("ur0", "ux0"):
            self.assertEqual(self.tai(partition), self.original_tai)
            self.assertFalse((self.device / partition / "tai/config.txt.vjo-bak").exists())
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_update_is_verified_idempotent_and_preserves_unrelated_state(self):
        self.assertFalse((self.package / "docs/config.example.ini").exists())
        installed = None
        # The second update names no game: the one registered by the first is kept.
        for options in (("--title", "PCSG00415"), ()):
            result = self.install("install-vita.sh", *options)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("test-key", result.stdout)
            self.assertNotIn("migration failed", result.stdout + result.stderr)
            self.assertIn("OCR worker registered for PCSG00415", result.stdout)
            if installed is not None:
                self.assertEqual([self.tai(p) for p in ("ur0", "ux0")], installed)
            else:
                # The Vita's own backup is the configuration from before the update.
                for partition in ("ur0", "ux0"):
                    self.assertEqual((self.device / partition / "tai/config.txt.vjo-bak").read_bytes(),
                                     self.original_tai)
            installed = [self.tai(p) for p in ("ur0", "ux0")]
        for partition in ("ur0", "ux0"):
            tai = self.tai(partition)
            self.assertEqual(tai.count(b"ur0:tai/VitaJPOverlay_Kernel.skprx"), 1)
            self.assertEqual(tai.count(b"ur0:tai/VitaJPOverlay_Shell.suprx"), 1)
            self.assertIn(b"ur0:tai/Other.skprx\r\n", tai)
            # The *ALL worker is commented out; the game's own section loads it.
            self.assertIn(b"*ALL\r\n# Mainline OCR: ur0:tai/VitaJPOverlay_OCR.suprx\r\n", tai)
            self.assertIn(b"\r\n*PCSG00415\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n", tai)
            self.assertEqual(tai.count(b"\nur0:tai/VitaJPOverlay_OCR.suprx"), 1)
            self.assertNotIn(b"\n", tai.replace(b"\r\n", b""))
        config = (self.data / "config.ini").read_text()
        for setting in ("dictionary = local", "ocr_backend = vocr", "vocr_model = VOCR3_H15_w8.vocr",
                        "vocr_fallback_model = VOCR3_FL10_w8.vocr", "log_file = on", "anki_mining = off",
                        "local_dictionaries = plaintext.vjdict", "font_size_ja = 24", "jpdb_api_key = test-key",
                        "ocr_lm = 1000k", "ocr_cascade_ms = 2000"):
            self.assertIn(setting, config)
        self.assertEqual((self.data / "games.ini").read_text(), "keep existing choices")
        self.assertEqual((self.data / "region.ini").read_text(), "keep existing region")
        self.assertTrue((self.device / "ux0/data/JPOverlaySettings.vpk").is_file())
        self.assertEqual((self.device / "ur0/tai/VitaJPOverlay_OCR.suprx").read_bytes(),
                         (self.package / "ur0/tai/VitaJPOverlay_OCR.suprx").read_bytes())
        for name in ("dictionaries/plaintext.vjdict", "ocr/FL10_w8.vocr", "ocr/H15_w8.vocr",
                     "ocr/VOCR3_H15_w8.vocr", "ocr/VOCR3_FL10_w8.vocr", "ocr/vocr_lm_o3_4000k.vlm",
                     "ocr/LM-ATTRIBUTION.md", "ocr/VOCR4_W20_w8.vocr"):
            self.assertEqual((self.data / name).read_bytes(),
                             (self.package / "ux0/data/VitaJPOverlay" / name).read_bytes())

    def test_package_settings_from_the_manifest_are_applied_after_the_defaults(self):
        # A test build's own settings (package_release.py --set): anki_mining = on wins over
        # the user's earlier off, and the installer says which settings the package brings.
        (self.data / "config.ini").write_text("anki_mining = off\nvocr_model = H15_w8.vocr\njpdb_api_key = test-key\n")
        manifest_path = self.package / "package-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["install_settings"] = {"anki_mining": "on", "anki_host": ""}
        manifest_path.write_text(json.dumps(manifest))
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("package settings: anki_mining=on anki_host=\n", result.stdout)
        config = (self.data / "config.ini").read_text()
        for setting in ("anki_mining = on\n", "anki_host =\n", "vocr_model = VOCR3_H15_w8.vocr\n",
                        "jpdb_api_key = test-key\n"):
            self.assertIn(setting, config)
        self.assertNotIn("test-key", result.stdout)

    def test_package_settings_are_validated_again_and_keys_are_masked(self):
        # A crafted manifest: no shell runs, a line break of any kind or an unknown key is dropped,
        # an API key's value is never printed.
        manifest_path = self.package / "package-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        canary = self.root / "canary"
        manifest["install_settings"] = {
            "anki_audio_url": f"$(touch {canary}) `touch {canary}` 'q' \"dq\"",
            "log_host": "line1\nlog_file = off", "hachidori_host": "a\u2028anki_host = 6.6.6.6",
            "anki_host": "x\x85y", "bad key": "1", "not_a_setting": "on", "anki_deck": "removed",
            "jpdb_api_key": "pkg-secret"}
        manifest_path.write_text(json.dumps(manifest))
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(canary.exists())
        self.assertNotIn("pkg-secret", result.stdout + result.stderr)
        self.assertIn("jpdb_api_key=set", result.stdout)
        shown = next(l for l in result.stdout.splitlines() if l.startswith("package settings:"))
        for dropped in ("log_host", "hachidori_host", "anki_host", "bad key", "not_a_setting", "anki_deck"):
            self.assertNotIn(dropped, shown)
        config = (self.data / "config.ini").read_text()
        self.assertIn(f"anki_audio_url = $(touch {canary}) `touch {canary}` 'q' \"dq\"\n", config)
        self.assertIn("jpdb_api_key = pkg-secret\n", config)
        self.assertIn("log_file = on\n", config)
        self.assertNotIn("6.6.6.6", config)
        self.assertNotIn("not_a_setting", config)

    def test_source_install_uses_the_checkout_template(self):
        # A source checkout has its template under docs, not at the package root.
        (self.package / "config.ini").unlink()
        (self.package / "docs").mkdir()
        shutil.copyfile(ROOT / "docs/config.example.ini", self.package / "docs/config.example.ini")
        build = self.package / "build/vita"
        build.mkdir(parents=True)
        for plugin in (self.package / "ur0/tai").iterdir():
            shutil.copyfile(plugin, build / plugin.name)
        shutil.copyfile(self.package / "ur0/data/VitaJPOverlay/vitajpoverlay.rco",
                        build / "vitajpoverlay.rco")
        result = self.install("tools/install_ftp.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        config = (self.data / "config.ini").read_text()
        for setting in ("ocr_backend = lens", "vocr_model = VOCR3_H15_w8.vocr",
                        "vocr_fallback_model = VOCR3_FL10_w8.vocr", "font_size_ja = 24"):
            self.assertIn(setting, config)
        self.assertNotIn("test-key", result.stdout)

    def test_source_install_rewrites_a_removed_engine_to_vocr(self):
        # The source installer keeps the saved engine (no --set ocr_backend), except a removed
        # one: the VOCR2 trial and ncnn (host tools) become the local engine, vocr.
        (self.package / "config.ini").unlink()
        (self.package / "docs").mkdir()
        shutil.copyfile(ROOT / "docs/config.example.ini", self.package / "docs/config.example.ini")
        build = self.package / "build/vita"
        build.mkdir(parents=True)
        for plugin in (self.package / "ur0/tai").iterdir():
            shutil.copyfile(plugin, build / plugin.name)
        shutil.copyfile(self.package / "ur0/data/VitaJPOverlay/vitajpoverlay.rco", build / "vitajpoverlay.rco")
        for removed in ("vocr2", "ncnn"):
            (self.data / "config.ini").write_text(
                "ocr_backend = %s\nvocr_model = H15_w8.vocr\nfont_size_ja = 24\njpdb_api_key = test-key\n" % removed)
            result = self.install("tools/install_ftp.sh", "--title", "PCSG00415")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            config = (self.data / "config.ini").read_text()
            self.assertIn("\nocr_backend = vocr\n", config)
            self.assertNotIn("ocr_backend = " + removed, config)
            for setting in ("vocr_model = H15_w8.vocr", "font_size_ja = 24", "jpdb_api_key = test-key"):
                self.assertIn(setting, config)
            self.assertIn("config.ini: ocr_backend = vocr", result.stdout)
            self.assertNotIn("test-key", result.stdout)

    def test_missing_source_template_stops_before_uploading_plugins(self):
        result = self.install("tools/install_ftp.sh", "--title", "PCSG00415")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing configuration template", result.stdout + result.stderr)
        self.assertFalse((self.device / "ur0/tai/VitaJPOverlay_Kernel.skprx").exists())
        self.assertEqual((self.device / "ur0/tai/config.txt").read_bytes(), self.original_tai)
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_migration_error_stops_before_publishing_configuration(self):
        template = self.package / "config.ini"
        template.write_bytes(b"invalid utf-8: \xff")
        # A checksum-valid package can still contain a template migration cannot read.
        manifest_path = self.package / "package-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["config.ini"] = {
            "bytes": template.stat().st_size,
            "sha256": hashlib.sha256(template.read_bytes()).hexdigest(),
        }
        manifest_path.write_text(json.dumps(manifest))
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("config.ini migration failed", result.stdout + result.stderr)
        self.assertNotIn("config.ini is up to date", result.stdout)
        self.assertNotIn("test-key", result.stdout + result.stderr)
        for partition in ("ur0", "ux0"):
            self.assertEqual((self.device / partition / "tai/config.txt").read_bytes(), self.original_tai)
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_bad_readback_stops_before_publishing_configuration(self):
        self.environment["CORRUPT_READBACK"] = "ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict"
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FTP readback checksum failed", result.stdout + result.stderr)
        self.assertEqual((self.device / "ur0/tai/config.txt").read_bytes(), self.original_tai)
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_without_a_title_nothing_is_changed(self):
        # The previous installer's *ALL registration names no game: refused, never kept.
        for options in ((), ("--title", "ALL"), ("--title", "NPXS10015"), ("--title", "PCSG0041")):
            with self.subTest(options=options):
                result = self.install("install-vita.sh", *options)
                self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
                self.assertIn("Nothing was changed on the Vita", result.stdout)
                self.assertIn("Correct the --title value" if options else "Run again with --title", result.stdout)
                self.assertIn("bash install-vita.sh --title PCSG00415 127.0.0.1", result.stdout)
                self.assertIn("--title" if options else "never registered under *ALL", result.stderr)
                self.assertNotIn("Installed.", result.stdout)
                self.assert_unchanged(result)

    def test_titles_are_added_and_kept_in_both_configurations(self):
        # ux0 already has one game; ur0 only the old *ALL line. Both end with the same games.
        (self.device / "ux0/tai/config.txt").write_bytes(
            b"*KERNEL\r\nur0:tai/Other.skprx\r\n*PCSE00120\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n")
        result = self.install("install-vita.sh", "--title", "NPXS10028", "--title", "pcsg00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # Both configurations report the same games, in the same order.
        self.assertEqual(result.stdout.count("(OCR worker registered for NPXS10028, PCSG00415, PCSE00120)"), 2)
        for partition in ("ur0", "ux0"):
            tai = self.tai(partition)
            for title in (b"NPXS10028", b"PCSG00415", b"PCSE00120"):
                self.assertIn(b"*" + title + b"\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n", tai)
            self.assertEqual(tai.count(b"\nur0:tai/VitaJPOverlay_OCR.suprx"), 3)
            self.assertNotIn(b"*ALL\r\nur0:tai/VitaJPOverlay_OCR.suprx", tai)
        # Options after the address work too, and change nothing more.
        before = [self.tai(p) for p in ("ur0", "ux0")]
        result = subprocess.run(["bash", str(self.package / "install-vita.sh"),
                                 "127.0.0.1:" + str(self.listener.getsockname()[1]), "--title", "PCSE00120"],
                                env=self.environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([self.tai(p) for p in ("ur0", "ux0")], before)

    def test_uninstall_after_a_per_title_install(self):
        self.assertEqual(self.install("install-vita.sh", "--title", "PCSG00415").returncode, 0)
        result = self.install("tools/install_ftp.sh", "--uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for partition in ("ur0", "ux0"):
            tai = self.tai(partition)
            self.assertIn(b"ur0:tai/Other.skprx\r\n", tai)
            for line in tai.split(b"\r\n"):
                self.assertFalse(line.startswith(b"ur0:tai/VitaJPOverlay_"), tai)
        # Data stays; the boot record is reset for the next build.
        self.assertEqual((self.data / "games.ini").read_text(), "keep existing choices")
        self.assertEqual((self.device / "ur0/data/VitaJPOverlay/boot.txt").read_bytes(), b"")
        # Nothing of ours is registered any more: the next install has to name its games.
        uninstalled = [self.tai(p) for p in ("ur0", "ux0")]
        result = self.install("install-vita.sh")
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertEqual([self.tai(p) for p in ("ur0", "ux0")], uninstalled)

    def test_self_test_accepts_a_title_and_leaves_the_configuration(self):
        # Issue #70's step: arming the self-test with --title changes no registration.
        result = self.install("tools/install_ftp.sh", "--self-test", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Self-test armed", result.stdout)
        # the command it names is the one that collects (install-vita.sh takes no --collect)
        self.assertIn("run: bash tools/install_ftp.sh --collect 127.0.0.1\n", result.stdout)
        self.assertNotIn("install-vita.sh --collect", result.stdout)
        self.assertIn("--title takes effect when installing (bash install-vita.sh --title PCSG00415", result.stdout)
        self.assertEqual((self.data / "selftest.txt").read_bytes(), b"")
        for partition in ("ur0", "ux0"):
            self.assertEqual(self.tai(partition), self.original_tai)
