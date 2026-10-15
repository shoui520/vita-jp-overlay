"""The release package's OCR models and their metadata, with tiny fixture bytes; no models,
network or device access."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ocr as prepare
import package_release as release


class PackageModels(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.models = self.root / "models"
        self.models.mkdir()
        self.legacy = b"tiny legacy asset fixture, not OCR weights!"
        old_spec = ("https://fixture.invalid/FL10_w8.vocr", len(self.legacy),
                    hashlib.sha256(self.legacy).hexdigest())
        old = {name: old_spec for name in ("FL10_w8.vocr", "H15_w8.vocr")}
        self.vocr3 = b"tiny VOCR3 asset fixture, not OCR weights!!"
        vocr3_spec = ("https://fixture.invalid/VOCR3", len(self.vocr3), hashlib.sha256(self.vocr3).hexdigest())
        vocr3 = {name: vocr3_spec for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr")}
        self.lm = b"tiny LM asset fixture, not a language model!"
        lm_spec = ("https://fixture.invalid/LM", len(self.lm), hashlib.sha256(self.lm).hexdigest())
        lms = {name: lm_spec for name in prepare.VOCR_LM_ASSETS}
        vocr4 = {name: vocr3_spec for name in prepare.VOCR4_ASSETS}
        self.attribution = b"CC BY-SA 4.0 fixture"
        att = {prepare.VOCR_LM_ATTRIBUTION: ("https://fixture.invalid/A", len(self.attribution),
                                             hashlib.sha256(self.attribution).hexdigest())}
        self.enterContext(mock.patch.object(release, "VOCR_LM_ATTRIBUTION_PIN", att))
        for module in (prepare, release):
            self.enterContext(mock.patch.object(module, "VOCR_ASSETS", old))
            self.enterContext(mock.patch.object(module, "VOCR3_ASSETS", vocr3))
            self.enterContext(mock.patch.object(module, "VOCR_LM_ASSETS", lms))
            self.enterContext(mock.patch.object(module, "VOCR4_ASSETS", vocr4))
        self.enterContext(mock.patch.object(prepare.urllib.request, "urlopen",
                                          side_effect=AssertionError("No test network access")))

    def write_models(self, attribution=None):
        for name in ("FL10_w8.vocr", "H15_w8.vocr"):
            (self.models / name).write_bytes(self.legacy)
        for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr"):
            (self.models / name).write_bytes(self.vocr3)
        for name in prepare.VOCR_LM_ASSETS:
            (self.models / name).write_bytes(self.lm)
        for name in prepare.VOCR4_ASSETS:
            (self.models / name).write_bytes(self.vocr3)
        (self.models / prepare.VOCR_LM_ATTRIBUTION).write_bytes(attribution or self.attribution)

    def package_fixture(self, settings=None, write_models=True):
        """The release package of a synthetic base archive and the fixture models (written first
        unless write_models is False); returns its path."""
        if write_models:
            self.write_models()
        dictionary = self.root / "jmdict.vjdict"
        dictionary.write_bytes(b"VJDICT2\0synthetic dictionary fixture")
        snapshot = {"bytes": dictionary.stat().st_size, "sha256": release.digest(dictionary)}
        license_file = self.root / "fixture-license.txt"
        license_file.write_text("Unit-test fixture")
        base = self.root / "base.zip"
        with zipfile.ZipFile(base, "w") as archive:
            for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                         "ur0/tai/VitaJPOverlay_OCR.suprx", "JPOverlaySettings.vpk",
                         "ur0/data/VitaJPOverlay/vitajpoverlay.rco"):
                archive.writestr(name, b"synthetic package fixture")
            archive.writestr("config.ini", b"; the build's own template copy\n")
        output = self.root / "package.zip"
        with mock.patch.object(release, "load_manifest", return_value=snapshot), \
             contextlib.redirect_stdout(io.StringIO()):
            release.package(base, output, self.models, dictionary, license_file, settings=settings)
        return output

    def test_bundle_ships_vocr3_as_default_and_v020_as_fallback(self):
        package = self.package_fixture()
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr", "H15_w8.vocr", "FL10_w8.vocr"):
                self.assertIn(release.DATA + "ocr/" + name, manifest["files"])
                self.assertIn(name, manifest["models"])
            self.assertEqual(sorted(manifest["models"]),
                             sorted(["FL10_w8.vocr", "H15_w8.vocr", "VOCR3_FL10_w8.vocr", "VOCR3_H15_w8.vocr",
                                     *prepare.VOCR_LM_ASSETS, *prepare.VOCR4_ASSETS]))
            self.assertIn(release.DATA + "ocr/LM-" + prepare.VOCR_LM_ATTRIBUTION, manifest["files"])
            self.assertEqual(manifest["models"]["VOCR3_H15_w8.vocr"]["release"], "bee-san/vita-vn-ocr v0.2.5-vocr3.1")
            self.assertEqual(manifest["models"]["VOCR3_H15_w8.vocr"]["role"], "default")
            self.assertEqual(manifest["models"]["H15_w8.vocr"]["release"], "bee-san/vita-vn-ocr v0.2.0")
            self.assertEqual(manifest["defaults"]["vocr_model"], "VOCR3_H15_w8.vocr")
            self.assertEqual(manifest["defaults"]["vocr_fallback_model"], "VOCR3_FL10_w8.vocr")
            self.assertTrue(manifest["defaults"]["log_file"])
            self.assertFalse(manifest["defaults"]["anki"])
            self.assertNotIn("install_settings", manifest)
            # both template copies are the source template: logging on, Anki off
            for name in ("config.ini", release.DATA + "config.ini"):
                text = archive.read(name).decode()
                self.assertIn("\nlog_file = on\n", text)
                self.assertIn("\nanki_mining = off\n", text)
                self.assertIn("\nvocr_model = VOCR3_H15_w8.vocr\n", text)

    def test_manifest_names_each_model_generation_and_the_local_engine(self):
        """The bundled metadata the Overlay app's label comes from: each model's generation (by its
        release), and the local engine's name for the package's default, VOCR3."""
        package = self.package_fixture()
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
        self.assertEqual({name: entry["generation"] for name, entry in manifest["models"].items() if "generation" in entry},
                         {"VOCR3_H15_w8.vocr": "VOCR3", "VOCR3_FL10_w8.vocr": "VOCR3",
                          "H15_w8.vocr": "VOCR1", "FL10_w8.vocr": "VOCR1", "VOCR4_W20_w8.vocr": "VOCR4"})
        for name in prepare.VOCR_LM_ASSETS:  # an LM is no recognizer: no generation
            self.assertNotIn("generation", manifest["models"][name])
        self.assertEqual(manifest["defaults"]["ocr_backend"], "vocr")
        self.assertEqual(manifest["defaults"]["ocr_engine"], "VOCR3")
        self.assertEqual(manifest["defaults"]["ocr_engine"],
                         manifest["models"][manifest["defaults"]["vocr_model"]]["generation"])
        # a later release's generation reaches the manifest through its tag
        with mock.patch.dict(prepare.VOCR_GENERATION, {prepare.VOCR3_TAG: "VOCR5"}), \
             mock.patch.dict(release.VOCR_GENERATION, {prepare.VOCR3_TAG: "VOCR5"}):
            (self.root / "package.zip").unlink()
            package = self.package_fixture()
            with zipfile.ZipFile(package) as archive:
                manifest = json.loads(archive.read("package-manifest.json"))
        self.assertEqual(manifest["defaults"]["ocr_engine"], "VOCR5")
        self.assertEqual(manifest["models"]["VOCR3_FL10_w8.vocr"]["generation"], "VOCR5")
        self.assertEqual(manifest["models"]["H15_w8.vocr"]["generation"], "VOCR1")

    def test_removed_vocr2_is_neither_shipped_nor_claimed(self):
        # a VOCR2 file left in the models directory is not packaged
        (self.models / "VOCR2_FL10_w8.vocr").write_bytes(self.legacy)
        package = self.package_fixture()
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertFalse([n for n in archive.namelist() if "VOCR2" in n])
        self.assertNotIn("ocr_backends", manifest)
        self.assertNotIn("VOCR2", json.dumps(manifest))
        for name in ("VOCR2_ENABLED", "VOCR2_MODEL", "VOCR2_TAG", "VOCR2_ASSETS", "fetch_vocr2"):
            self.assertFalse(hasattr(prepare, name), name)
            self.assertFalse(hasattr(release, name), name)

    def test_package_settings_go_into_both_templates_and_the_manifest(self):
        package = self.package_fixture({"anki_mining": "on", "log_file": "on"})
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertEqual(manifest["install_settings"], {"anki_mining": "on", "log_file": "on"})
            self.assertTrue(manifest["defaults"]["anki"])
            for name in ("config.ini", release.DATA + "config.ini"):
                text = archive.read(name).decode()
                self.assertIn("\nanki_mining = on\n", text)
                self.assertNotIn("anki_mining = off", text)
                self.assertEqual(manifest["files"][name]["sha256"], hashlib.sha256(text.encode()).hexdigest())
        # a key the template does not have, or a value with a line break, is refused before output
        with self.assertRaisesRegex(ValueError, "Not a setting of the config template: anki_minnig"):
            self.package_fixture({"anki_minnig": "on"})
        for bad in ("anki_mining", "anki_mining=on\nlog_file=off", "=on", "Bad Key=1", "anki_deck=a\u2028b",
                    "anki_deck=a\x85b", "anki_deck=a\x7fb"):
            with self.assertRaises(Exception):
                release.parse_setting(bad)
        self.assertEqual(release.parse_setting(" Anki_Mining = on "), ("anki_mining", "on"))
        self.assertEqual(release.parse_setting("anki_field_picture="), ("anki_field_picture", ""))

    def manifest(self, settings=None):
        with zipfile.ZipFile(self.package_fixture(settings)) as archive:
            return json.loads(archive.read("package-manifest.json")), archive.read("config.ini").decode()

    def test_the_manifest_defaults_are_the_config_templates_settings(self):
        """audit-test-build-2 N4: the defaults hold every setting of the config.ini template, each with the
        template's value (a switch as true/false, a number as an integer), so a setting added to the
        template is in the next manifest without another edit; the API 20 job keys included."""
        manifest, shipped_template = self.manifest()
        template = release.template_settings((ROOT / "docs/config.example.ini").read_text(encoding="utf-8"))
        self.assertEqual(release.template_settings(shipped_template), template)
        defaults = manifest["defaults"]
        self.assertEqual(set(defaults) - {"ocr_engine", "anki"}, set(template))
        self.assertEqual(list(defaults)[:len(template)], list(template))  # in the template's order
        for key, value in template.items():
            got = defaults[key]
            if key in release.SWITCHES:
                self.assertIs(got, {"on": True, "off": False}[value], key)
            elif key in release.NUMBERS:
                self.assertEqual(got, int(value), key)
            elif release.SECRET.search(key):
                self.assertEqual(got, "set" if value else "", key)
            else:
                self.assertEqual(got, value, key)
        # every setting the template has is classified as core/config.c parses it
        self.assertEqual({k for k in template if template[k] in ("on", "off") and k != "ocr_refine_lines"},
                         set(release.SWITCHES))
        self.assertEqual({k for k in template if template[k].isdigit()}, set(release.NUMBERS))
        # the API 20 job keys, as the template and core/config.c's defaults have them
        self.assertEqual({k: defaults[k] for k in ("ocr_settle_ms", "ocr_settle_max_ms", "ocr_skip_blank_lines",
                                                   "ocr_refine_lines", "ocr_lm", "ocr_cascade_ms")},
                         {"ocr_settle_ms": 150, "ocr_settle_max_ms": 2000, "ocr_skip_blank_lines": False,
                          "ocr_refine_lines": "psp", "ocr_lm": "1000k", "ocr_cascade_ms": 2000})
        # what the manifest said before is still said the same way
        self.assertEqual({k: defaults[k] for k in ("ocr_backend", "ocr_engine", "vocr_model", "vocr_fallback_model",
                                                   "dictionary", "local_dictionaries", "japanese_only_selection",
                                                   "ocr_mode", "log_file", "anki")},
                         {"ocr_backend": "vocr", "ocr_engine": "VOCR3", "vocr_model": "VOCR3_H15_w8.vocr",
                          "vocr_fallback_model": "VOCR3_FL10_w8.vocr", "dictionary": "local",
                          "local_dictionaries": "plaintext.vjdict", "japanese_only_selection": True,
                          "ocr_mode": "auto", "log_file": True, "anki": False})

    def test_package_settings_reach_the_defaults_and_credentials_do_not(self):
        manifest, _ = self.manifest({"anki_mining": "on", "highlight_mode": "character", "ocr_settle_ms": "0",
                                     "ocr_refine_lines": "off", "ocr_skip_blank_lines": "on",
                                     "jpdb_api_key": "fixture-secret-key", "anki_audio_url": "https://a.invalid/?k=s"})
        defaults = manifest["defaults"]
        self.assertEqual({k: defaults[k] for k in ("anki_mining", "anki", "highlight_mode", "ocr_settle_ms",
                                                   "ocr_refine_lines", "ocr_skip_blank_lines")},
                         {"anki_mining": True, "anki": True, "highlight_mode": "character", "ocr_settle_ms": 0,
                          "ocr_refine_lines": "off", "ocr_skip_blank_lines": True})
        self.assertEqual((defaults["jpdb_api_key"], defaults["jiten_api_key"], defaults["anki_audio_url"]),
                         ("set", "", "set"))
        self.assertNotIn("fixture-secret-key", json.dumps(defaults))
        self.assertNotIn("a.invalid", json.dumps(defaults))
        # a value core/config.c would not read as its type is refused before any output
        for bad in ({"log_file": "yes"}, {"ocr_cascade_ms": "2s"}):
            with self.assertRaises(ValueError):
                self.package_fixture(bad)
        with self.assertRaisesRegex(ValueError, "sets ocr_lm twice"):
            release.template_settings("ocr_lm = 1000k\nocr_lm = off\n")
        with self.assertRaisesRegex(ValueError, "not one of the package's recognizers"):
            release.manifest_defaults("vocr_model = VOCR9_w8.vocr\n", {})

    def test_bundle_refuses_an_attribution_that_is_not_the_pinned_one(self):
        # Another release's ATTRIBUTION.md (ctclm.1's lacks the protected-title paragraph) is refused.
        for other in (b"CC BY-SA 4.0 fixture of an older release", b"CC BY-SA 4.0 fixturE"):  # its size; the same size
            self.write_models(attribution=other)
            with self.assertRaisesRegex(ValueError, "ATTRIBUTION.md checksum failed"):
                self.package_fixture(write_models=False)
            self.assertFalse((self.root / "package.zip").exists())

    def test_missing_or_wrong_model_is_refused_before_output(self):
        self.write_models()
        (self.models / "VOCR3_FL10_w8.vocr").unlink()
        with self.assertRaisesRegex(ValueError, "OCR model is missing: VOCR3_FL10_w8.vocr"):
            self.package_fixture(write_models=False)
        self.assertFalse((self.root / "package.zip").exists())
        (self.models / "VOCR3_FL10_w8.vocr").write_bytes(self.legacy)  # the right size, another hash
        with self.assertRaisesRegex(ValueError, "OCR model checksum failed: VOCR3_FL10_w8.vocr"):
            self.package_fixture(write_models=False)
        self.assertFalse((self.root / "package.zip").exists())

if __name__ == "__main__":
    unittest.main()
