# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "scripts/manage.py"
spec = importlib.util.spec_from_file_location("theme_manager", SOURCE)
manager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manager)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kate-theme-test-")
        self.root = Path(self.temp.name)
        self.changes = manager.Changes(self.root / "state")

    def tearDown(self):
        self.temp.cleanup()

    def test_edit_preserves_unrelated_private_content_and_nested_sections(self):
        original = "# comment\n[Private]\nToken=unchanged\n\n[General]\nShow Menu Bar=false\n\n[Group][Nested]\nKey=keep\n"
        changed = manager.update_setting(original, "General", "Show Menu Bar", "true")
        self.assertEqual(changed.replace("Show Menu Bar=true", "Show Menu Bar=false"), original)
        self.assertEqual(manager.setting(changed, "Group][Nested", "Key"), "keep")

    def test_reinstall_keeps_original_backup_and_uninstall_preserves_new_keys(self):
        path = self.root / "katerc"
        path.write_text("[General]\nShow Menu Bar=false\n[Private]\nToken=keep\n")
        self.changes.patch(path, "General", "Show Menu Bar", "true")
        self.changes.patch(path, "General", "Show Menu Bar", "true")
        path.write_text(path.read_text() + "AddedLater=still-here\n")
        self.assertEqual(self.changes.uninstall(), [])
        restored = path.read_text()
        self.assertEqual(manager.setting(restored, "General", "Show Menu Bar"), "false")
        self.assertIn("Token=keep", restored)
        self.assertIn("AddedLater=still-here", restored)

    def test_changed_settings_and_files_are_not_overwritten_on_uninstall(self):
        config = self.root / "katerc"
        asset = self.root / "style.qss"
        asset.write_text("original")
        self.changes.patch(config, "Renderer", "Theme", "GitHub Black")
        self.changes.put(asset, "installed")
        config.write_text("[Renderer]\nTheme=My New Theme\n")
        asset.write_text("my edits")
        skipped = self.changes.uninstall()
        self.assertEqual(len(skipped), 2)
        self.assertEqual(asset.read_text(), "my edits")
        self.assertEqual(manager.setting(config.read_text(), "Renderer", "Theme"), "My New Theme")
        self.changes.uninstall(force=True)
        self.assertEqual(asset.read_text(), "original")

    def test_managed_file_restores_original_across_updates(self):
        path = self.root / "launcher"
        path.write_text("original")
        path.chmod(0o700)
        self.changes.put(path, "version one", 0o755)
        self.changes.put(path, "version two", 0o755)
        self.assertEqual(self.changes.uninstall(), [])
        self.assertEqual(path.read_text(), "original")
        self.assertEqual(path.stat().st_mode & 0o777, 0o700)

    def test_config_symlink_is_preserved(self):
        actual = self.root / "dotfiles/katerc"
        actual.parent.mkdir()
        actual.write_text("[UiSettings]\nColorScheme=Before\n")
        link = self.root / "katerc"
        link.symlink_to(actual)
        self.changes.patch(link, "UiSettings", "ColorScheme", "TerminalBlack")
        self.assertTrue(link.is_symlink())
        self.assertEqual(self.changes.uninstall(), [])
        self.assertTrue(link.is_symlink())
        self.assertIn("ColorScheme=Before", actual.read_text())

    def test_tmux_include_uninstall_keeps_other_edits(self):
        path = self.root / "tmux.conf"
        path.write_text("set -g mouse on\n")
        self.changes.block(path, 'source-file "/tmp/a theme.conf"')
        self.changes.block(path, 'source-file "/tmp/a theme.conf"')
        self.assertEqual(path.read_text().count(manager.BLOCK_START), 1)
        path.write_text(path.read_text() + "# added after installation\n")
        self.assertEqual(self.changes.uninstall(), [])
        self.assertIn("set -g mouse on", path.read_text())
        self.assertIn("# added after installation", path.read_text())
        self.assertNotIn(manager.BLOCK_START, path.read_text())

    def test_legacy_plugin_is_restored_on_uninstall(self):
        path = self.root / "old-plugin.so"
        path.write_bytes(b"original")
        self.changes.remove_legacy(path)
        self.assertFalse(path.exists())
        self.assertEqual(self.changes.uninstall(), [])
        self.assertEqual(path.read_bytes(), b"original")


if __name__ == "__main__":
    unittest.main()
