#!/usr/bin/env python3
"""Offline launcher release regression tests; no credentials or network used."""
import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location(
    "publisher", Path(__file__).resolve().parents[1] / "publish_release.py")
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class PublicationTagTest(unittest.TestCase):
    """Exercise publication with the real write wrapper and mocked API calls."""
    def setUp(self):
        self.gh = publisher.GitHub('offline', dry=False)
        self.rel = dict(id=42, tag_name='v0.7.0.7')
        self.url = f'/repos/{publisher.REPO}/releases/42'
        self.transport = self.enterContext(patch.object(
            publisher.urllib.request, 'urlopen', side_effect=AssertionError('network')))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    def test_publish_reads_back_matching_tag(self):
        for latest in (False, True):
            with self.subTest(latest=latest):
                self.gh.call = Mock(side_effect=[{}, dict(self.rel)])
                publisher.published(self.gh, self.rel, latest)
                self.assertEqual(self.gh.call.call_args_list, [
                    unittest.mock.call('PATCH', self.url, body={
                        'draft': False, 'make_latest': 'true' if latest else 'false',
                        'tag_name': self.rel['tag_name']}),
                    unittest.mock.call('GET', self.url)])

    def test_wrong_tag_fails_with_repair_details(self):
        for wrong in ('untagged-af3adbbc86d2df64365f', 'v0.7.0.6'):
            with self.subTest(tag=wrong):
                self.gh.call = Mock(side_effect=[{}, dict(tag_name=wrong)])
                with self.assertRaises(SystemExit) as raised:
                    publisher.published(self.gh, self.rel, True)
                message = str(raised.exception)
                self.assertIn('published, but GitHub reports its tag', message)
                self.assertIn(repr(wrong), message)
                self.assertIn("PATCH /releases/42 with tag_name 'v0.7.0.7'", message)
                self.assertEqual(self.gh.call.call_count, 2)

    def test_dry_run_never_contacts_service(self):
        self.gh.dry = True
        publisher.published(self.gh, self.rel, True)
        self.transport.assert_not_called()


class LauncherReleaseTest(unittest.TestCase):
    def setUp(self):
        self.args = SimpleNamespace(windows_launcher=None, linux_launcher=None,
                                    no_linux_launcher=False, publish=False, dry_run=False)
        self.blobs = {"launcher-Setup.exe": b"windows fixture", "launcher.AppImage": b"linux fixture"}
        self.blobs["SHA256SUMS.txt"] = "".join(
            f"{hashlib.sha256(data).hexdigest()}  {name}\n"
            for name, data in self.blobs.items()).encode()
        self.source = {"tag_name": "v1.2.3", "html_url": "https://example.invalid/launcher",
                       "assets": [{"name": n, "browser_download_url": n} for n in self.blobs]}
        self.existing = []
        self.writes = []
        self.gh = SimpleNamespace(call=self.call, write=self.write)
        # Unexpected credential access and HTTP calls fail, including future code paths.
        self.enterContext(patch.object(publisher, "token", side_effect=AssertionError("credentials")))
        self.enterContext(patch.object(publisher.GitHub, "call", side_effect=AssertionError("network")))
        self.enterContext(patch.object(publisher.urllib.request, "urlopen",
                                      side_effect=lambda url, **kw: io.BytesIO(self.blobs[url])))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    def call(self, method, url):
        self.assertEqual(method, "GET")
        if url == f"/repos/{publisher.LAUNCHER_REPO}/releases/latest":
            return self.source
        self.assertTrue(url.startswith(f"/repos/{publisher.REPO}/releases?"), url)
        return self.existing

    def write(self, what, method, url, **kw):
        self.writes.append((method, url, kw))
        if self.args.dry_run:
            return None
        if method == "POST" and url.endswith("/releases"):
            return {**kw["body"], "id": 42, "assets": [],
                    "upload_url": "https://example.invalid/upload{?name}",
                    "html_url": "https://example.invalid/release"}

    def run_release(self):
        publisher.launcher_release(self.gh, self.args)

    def test_draft_contains_both_verified_launchers(self):
        self.run_release()
        body = self.writes[0][2]["body"]
        self.assertEqual(body["tag_name"], "launcher-v1.2.3")
        self.assertEqual(body["make_latest"], "false")
        self.assertTrue(body["draft"])
        uploads = self.writes[1:]
        self.assertEqual(len(uploads), 2)
        for (method, url, kw), name, data in zip(uploads,
                (publisher.WINDOWS_NAME, publisher.LINUX_NAME), list(self.blobs.values())[:2]):
            self.assertEqual(method, "POST")
            self.assertTrue(url.endswith("?name=" + name))
            self.assertEqual(kw["data"], data)
            self.assertIn("/launcher-v1.2.3/" + name, body["body"])

    def test_publish_never_marks_latest(self):
        self.args.publish = True
        self.run_release()
        self.assertEqual(self.writes[-1], ("PATCH", f"/repos/{publisher.REPO}/releases/42",
                         {"body": {"draft": False, "make_latest": "false", "tag_name": "launcher-v1.2.3"}}))

    def test_launcher_update_leaves_existing_mod_release_intact(self):
        self.existing = [dict(tag_name='v0.7.0.5', draft=False, id=7)]
        self.args.publish = True
        self.run_release()
        self.assertFalse(any(method == 'DELETE' or url.endswith('/releases/7')
                             for method, url, _ in self.writes))

    def test_bad_linux_checksum_stops_before_writes(self):
        self.blobs["launcher.AppImage"] += b"damaged"
        with self.assertRaisesRegex(SystemExit, "checksum"):
            self.run_release()
        self.assertEqual(self.writes, [])

    def test_published_release_is_untouched(self):
        self.existing = [{"tag_name": "launcher-v1.2.3", "draft": False}]
        with self.assertRaisesRegex(SystemExit, "already published"):
            self.run_release()
        self.assertEqual(self.writes, [])

    def test_dry_run_does_not_upload(self):
        self.args.dry_run = True
        self.args.publish = True
        self.run_release()
        self.assertEqual(len(self.writes), 1)  # Only the suppressed draft creation.

    def test_windows_only_override(self):
        self.args.no_linux_launcher = True
        self.run_release()
        self.assertEqual(len(self.writes), 2)
        self.assertNotIn(publisher.LINUX_NAME, self.writes[0][2]["body"]["body"])

    def test_invalid_launcher_version_rejected(self):
        self.source["tag_name"] = "nightly"
        with self.assertRaisesRegex(SystemExit, "unexpected launcher tag"):
            self.run_release()
        self.assertEqual(self.writes, [])

    def test_mod_still_requires_linux_directory_before_credentials(self):
        with patch.object(sys, "argv", ["publish_release.py", "v0.7.0.6"]):
            with self.assertRaisesRegex(SystemExit, "needs --linux-dir"):
                publisher.main()

    def test_launcher_cli_needs_no_linux_payload(self):
        with patch.object(sys, "argv", ["publish_release.py", "launcher"]), \
             patch.object(publisher, "token", return_value="offline"), \
             patch.object(publisher, "GitHub", return_value=self.gh):
            publisher.main()
        self.assertEqual(len(self.writes), 3)


class VersionReleaseTest(unittest.TestCase):
    """Exercise the full publisher with local payloads and an in-memory service."""
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[2] / '.git')))
        self.version = '0.7.0.6'
        self.files = {
            'TpF2Multiplayer.msi': b'msi fixture',
            'TpF2Multiplayer-files.zip': b'zip fixture',
            publisher.SERVER_NAME: b'server fixture',
            'install_proton.py': b'DEFAULT_VERSION = "0.7.0.6"',
            'install_proton.sh': b'DEFAULT_VERSION="0.7.0.6"'}
        self.files['SHA256SUMS.txt'] = self.checksums(self.files)
        native = {f'tpf2mp-linux-{self.version}-native.{ext}': ext.encode()
                  for ext in ('run', 'tar.gz')}
        native[f'tpf2mp-linux-{self.version}-native.sha256'] = self.checksums(native)
        self.files.update(native)
        for name, data in self.files.items():
            (self.root / name).write_bytes(data)
        for name in (publisher.WINDOWS_NAME, publisher.LINUX_NAME):
            (self.root / name).write_bytes(name.encode())
        self.mod = self.release(1, 'v' + self.version)
        self.mod['body'] = '| **Linux / Steam Deck** | https://example.invalid/releases/download/v0.7.0.6/' + publisher.LINUX_NAME + ' |'
        self.mod['assets'] = [{'id': 90, 'name': publisher.LINUX_NAME},
                              {'id': 91, 'name': 'obsolete-payload.zip'}]
        self.releases = [self.mod]
        self.writes = []
        self.gh = SimpleNamespace(call=self.call, write=self.write, get=lambda url: None)
        self.enterContext(patch.object(publisher, 'token', return_value='offline'))
        self.enterContext(patch.object(publisher, 'GitHub', return_value=self.gh))
        self.enterContext(patch.object(publisher.urllib.request, 'urlopen', side_effect=AssertionError('network')))
        self.sleep = self.enterContext(patch('time.sleep'))   # the publisher must never wait
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    @staticmethod
    def checksums(files):
        return ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                       for name, data in files.items()).encode()

    @staticmethod
    def release(id, tag):
        return dict(id=id, tag_name=tag, draft=True, prerelease=False, assets=[],
                    name='TpF2 Multiplayer', body='', target_commitish='fixture-commit',
                    html_url=f'https://example.invalid/{tag}',
                    upload_url=f'https://example.invalid/upload/{id}' + '{?name}')

    def call(self, method, url):
        self.assertEqual(method, 'GET')
        if '/git/ref/tags/' in url:
            return {'object': {'type': 'commit', 'sha': 'fixture-commit'}}
        if url == f'/repos/{publisher.LAUNCHER_REPO}/releases/latest':
            return dict(tag_name='v1.3.0', assets=[])
        self.assertTrue(url.startswith(f'/repos/{publisher.REPO}/releases?'), url)
        return self.releases

    def write(self, what, method, url, **kw):
        self.writes.append((method, url, kw))
        if method == 'POST' and url.endswith('/git/tags'):
            return {'sha': 'page-tag-object'}
        if method == 'POST' and url.endswith('/releases'):
            return {**self.release(10 + len(self.writes), kw['body']['tag_name']), **kw['body']}

    def run_version(self, *flags):
        argv = ['publish_release.py', 'v' + self.version, '--linux-dir', str(self.root),
                '--payload-dir', str(self.root), '--windows-launcher', str(self.root / publisher.WINDOWS_NAME),
                '--linux-launcher', str(self.root / publisher.LINUX_NAME), *flags]
        with patch.object(sys, 'argv', argv):
            publisher.main()

    def test_native_files_on_both_install_releases_and_page_published_last(self):
        self.run_version('--publish')
        uploads = [(url, kw['data']) for method, url, kw in self.writes if '?name=' in url]
        # every install file to both install releases, the two launchers, and the direct
        # installers once more on the version's page
        direct = publisher.page_direct(self.version)
        self.assertEqual(len(uploads), 2 * len(self.files) + 2 + len(direct))
        for name, data in self.files.items():
            want = 3 if name in direct else 2
            self.assertEqual([b for url, b in uploads if url.endswith('?name=' + name)], [data] * want)
        for name in (publisher.WINDOWS_NAME, publisher.LINUX_NAME):
            self.assertEqual(sum(url.endswith('?name=' + name) for url, _ in uploads), 1)
        published = [(url, kw['body']) for method, url, kw in self.writes
                     if method == 'PATCH' and 'draft' in kw['body']]
        self.assertEqual([body for _, body in published], [
            {'draft': False, 'make_latest': 'false', 'tag_name': '0.7.0.6'},
            {'draft': False, 'make_latest': 'true', 'tag_name': 'v0.7.0.6'}])
        self.assertTrue(published[-1][0].endswith('/1'))
        self.sleep.assert_not_called()
        page = next(kw['body'] for method, url, kw in self.writes
                    if method == 'POST' and url == f'/repos/{publisher.REPO}/releases')
        self.assertEqual(page['tag_name'], self.version)
        self.assertIn('/releases/tag/v0.7.0.6', page['body'])
        self.assertEqual(page['target_commitish'], 'fixture-commit')
        self.assertFalse(any('/git/' in url for _, url, _ in self.writes))
        # Existing launcher is replaced; of the install files only the direct installers go to the page.
        self.assertIn(('DELETE', f'/repos/{publisher.REPO}/releases/assets/90', {}), self.writes)
        page_uploads = [url.split('?name=')[1] for url, _ in uploads if '/upload/1?' in url]
        self.assertEqual(set(page_uploads), {publisher.WINDOWS_NAME, publisher.LINUX_NAME, *direct})
        self.assertIn(('DELETE', f'/repos/{publisher.REPO}/releases/assets/91', {}), self.writes)

    def test_damaged_server_package_stops_before_upload(self):
        (self.root / publisher.SERVER_NAME).write_bytes(b'damaged')
        with self.assertRaisesRegex(SystemExit, 'SHA256SUMS'):
            self.run_version('--publish')
        self.assertEqual(self.writes, [])

    def test_prerelease_page_is_not_latest(self):
        self.mod['prerelease'] = True
        self.run_version('--publish')
        for _, _, kw in self.writes:
            if 'make_latest' in kw.get('body', {}):
                self.assertEqual(kw['body']['make_latest'], 'false')

    def test_default_leaves_both_as_drafts(self):
        self.run_version()
        self.assertFalse(any(kw.get('body', {}).get('draft') is False for _, _, kw in self.writes))
        self.sleep.assert_not_called()

    def test_corrupt_native_file_prevents_all_writes(self):
        (self.root / f'tpf2mp-linux-{self.version}-native.run').write_bytes(b'damaged')
        with self.assertRaisesRegex(SystemExit, 'does not match'):
            self.run_version('--publish')
        self.assertEqual(self.writes, [])

    def test_published_page_is_protected(self):
        self.mod['draft'] = False
        with self.assertRaisesRegex(SystemExit, 'already published'):
            self.run_version('--publish')
        self.assertEqual(self.writes, [])

    def test_windows_only_page_links_proton_fallback(self):
        self.run_version('--no-linux-launcher')
        page = next(kw['body'] for method, url, kw in self.writes
                    if method == 'PATCH' and url.endswith('/releases/1'))
        self.assertNotIn(publisher.LINUX_NAME, page['body'])
        self.assertIn(f'{publisher.PACKAGES_REPO}/releases/download/v0.7.0.6/install_proton.sh', page['body'])


class PageReleaseTest(LauncherReleaseTest):
    def prepare(self):
        self.gh.get = lambda url: None
        self.args.page_tag = 'v0.7.0.5'
        self.notes = '## Download\n\nold table\n\n## Changes\n\nVersion notes.\n'
        self.mod = VersionReleaseTest.release(7, self.args.page_tag)
        self.mod.update(draft=False, body=self.notes)
        self.payload = {name: ('fixture ' + name).encode() for name in (
            *publisher.PAYLOAD, 'tpf2mp-linux-0.7.0.5-native.run',
            'tpf2mp-linux-0.7.0.5-native.tar.gz', 'tpf2mp-linux-0.7.0.5-native.sha256')}
        self.blobs.update(self.payload)
        self.mod['assets'] = [dict(id=100+i, name=name, browser_download_url=name,
                                  digest='sha256:' + hashlib.sha256(data).hexdigest())
                              for i, (name, data) in enumerate(self.payload.items())]
        self.existing = [self.mod]

    def call(self, method, url):
        if url.endswith('/git/ref/tags/v0.7.0.5'):
            return {'object': {'type': 'commit', 'sha': 'version-commit'}}
        return super().call(method, url)

    def run_page(self):
        publisher.page_release(self.gh, self.args)

    def test_migration_copies_all_payloads_before_removing_original_assets(self):
        self.prepare()
        self.args.publish = True
        self.run_page()
        creations = [kw['body'] for method, url, kw in self.writes
                     if method == 'POST' and url.endswith('/releases')]
        self.assertEqual([body['tag_name'] for body in creations], ['v0.7.0.5', '0.7.0.5'])
        self.assertEqual(creations[1]['target_commitish'], 'version-commit')
        uploads = [(url.split('?name=')[1], kw['data']) for _, url, kw in self.writes if '?name=' in url]
        for name, data in self.payload.items():
            self.assertEqual([blob for n, blob in uploads if n == name], [data, data])
        first_delete = next(i for i, (method, _, _) in enumerate(self.writes) if method == 'DELETE')
        # every payload file to both install releases, then the two launchers
        self.assertEqual(sum('?name=' in url for _, url, _ in self.writes[:first_delete]), 2 * len(self.payload) + 2)
        self.assertTrue(all('/releases/assets/' in url for method, url, _ in self.writes if method == 'DELETE'))
        final = self.writes[-1]
        self.assertTrue(final[1].endswith('/releases/7'))
        self.assertEqual(final[2]['body']['make_latest'], 'true')
        self.assertEqual(final[2]['body']['name'], '0.7.0.5')
        body = final[2]['body']['body']
        self.assertNotIn('old table', body)
        self.assertTrue(body.endswith('## Changes\n\nVersion notes.\n'))
        for name in (publisher.WINDOWS_NAME, publisher.LINUX_NAME):
            self.assertIn('/download/v0.7.0.5/' + name, body)
        publications = [kw['body'] for method, _, kw in self.writes
                        if method == 'PATCH' and 'draft' in kw['body']]
        self.assertEqual(publications, [{'draft': False, 'make_latest': 'false', 'tag_name': '0.7.0.5'}])

    def test_corrupt_native_digest_prevents_writes(self):
        self.prepare()
        self.blobs['tpf2mp-linux-0.7.0.5-native.run'] += b'corrupt'
        with self.assertRaisesRegex(SystemExit, 'does not match its digest'):
            self.run_page()
        self.assertEqual(self.writes, [])

    def test_failed_copy_upload_prevents_original_asset_removal(self):
        self.prepare()
        original = self.gh.write
        copies = 0
        def fail_upload(what, method, url, **kw):
            nonlocal copies
            if '?name=TpF2Multiplayer.msi' in url:
                copies += 1
                if copies == 2:
                    raise OSError('fixture upload failure')
            return original(what, method, url, **kw)
        self.gh.write = fail_upload
        with self.assertRaisesRegex(OSError, 'fixture upload failure'):
            self.run_page()
        self.assertFalse(any(method == 'DELETE' for method, _, _ in self.writes))

    def test_migrated_page_uses_existing_update_files(self):
        self.prepare()
        self.mod['assets'] = []
        copy = VersionReleaseTest.release(8, '0.7.0.5')
        copy.update(draft=False, assets=[{'name': 'TpF2Multiplayer.msi'}])
        self.existing.append(copy)
        self.run_page()
        self.assertFalse(any(method == 'DELETE' or url.endswith('/releases')
                             for method, url, _ in self.writes))
        self.assertEqual(sum('?name=' in url for _, url, _ in self.writes), 2)

    def test_prerelease_page_never_latest(self):
        self.prepare()
        self.mod['prerelease'] = True
        self.args.publish = True
        self.run_page()
        for _, _, kw in self.writes:
            if 'make_latest' in kw.get('body', {}):
                self.assertEqual(kw['body']['make_latest'], 'false')

    def test_invalid_unpublished_or_missing_payload_rejected(self):
        for tag, draft, assets in [('invalid', False, True), ('v0.7.0.5', True, True),
                                    ('v0.7.0.5', False, False)]:
            with self.subTest(tag=tag, draft=draft, assets=assets):
                self.prepare()
                self.args.page_tag = tag
                self.mod['draft'] = draft
                if not assets:
                    self.mod['assets'] = []
                with self.assertRaises(SystemExit):
                    self.run_page()
                self.assertEqual(self.writes, [])

    def test_page_cli_needs_no_linux_directory(self):
        with patch.object(sys, 'argv', ['publish_release.py', 'page', 'v0.7.0.5']), \
             patch.object(publisher, 'token', return_value='offline'), \
             patch.object(publisher, 'GitHub', return_value=self.gh), \
             patch.object(publisher, 'page_release') as run:
            publisher.main()
        self.assertEqual(run.call_args.args[1].page_tag, 'v0.7.0.5')

    def test_dry_run_suppresses_upload_and_delete_calls(self):
        self.prepare()
        self.args.dry_run = self.args.publish = True
        # Use the real dry-run writer; any accidental real call fails immediately.
        gh = publisher.GitHub('offline', True)
        gh.call = self.call
        gh.get = lambda url: None
        with patch.object(gh, 'call', wraps=self.call) as calls:
            publisher.page_release(gh, self.args)
        self.assertTrue(all(call.args[0] == 'GET' for call in calls.call_args_list))
        self.assertEqual(self.writes, [])

    def test_previous_note_removed(self):
        self.prepare()
        self.mod['body'] = 'The install files of **previous page**.\n\n' + self.notes
        self.run_page()
        self.assertNotIn('previous page', self.writes[-1][2]['body']['body'])


if __name__ == '__main__':
    unittest.main()
