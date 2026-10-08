"""实际包边界、复算和所有权接缝测试；不启动网络或导入旧工具链。"""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import stat
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock
import subprocess


def load_module(package, name):
    path = package / (name + '.py')
    spec = importlib.util.spec_from_file_location('baseline_test_' + name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def create_suite(package, scratch):
    prepare = load_module(package, 'prepare_package')
    verifier = load_module(package, 'verify_sample')
    owned = load_module(package, 'owned_process')

    class PackageTests(unittest.TestCase):
        def make_sample_fixture(self):
            root = Path(tempfile.mkdtemp(prefix='sample-fixture-', dir=scratch))
            histograms = [{500:512}, {500:256}, {500:256}, {}, {}, {500:512}, {500:256}]
            client = {'schema':'baseline-v1-client','status':'valid','clock':'CLOCK_MONOTONIC',
                      'warm_start_ns':100,'T0_ns':1000000100,'T1_ns':4000000100,
                      'duration_ns':3000000000,'N':512,'qps':512/3,'interval_us':[750000,1500000],
                      'histograms':[], 'threads':[]}
            request = (package/'config/request.bin').read_bytes()
            runtime_path = str(package/'runtime/libluajit-5.1.so.2')
            dependencies = [{'path':runtime_path,'sha256':hashlib.sha256(Path(runtime_path).read_bytes()).hexdigest()}]
            client.update(pid=12346,uid=1000,gid=1000,request_bytes=len(request),request_sha256=hashlib.sha256(request).hexdigest(),
                          actual_LD_LIBRARY_PATH=str(package/'runtime'),
                          actual_LUA_PATH=str(package/'runtime/lua/?.lua')+';'+str(package/'runtime/lua/?/init.lua'),
                          actual_luajit_library=runtime_path)
            for index, bins in enumerate(histograms):
                name = verifier.NAMES[index]
                (root/name).write_bytes(b''.join(struct.pack('<QQ', latency, count) for latency,count in sorted(bins.items())))
                minimum = min(bins, default=(1<<64)-1)
                client['histograms'].append({'file':name,'count':sum(bins.values()),'max_us':max(bins,default=0),
                                             'scan_min_us':minimum,'p50_us':500 if bins else 0,'p99_us':500 if bins else 0})
            for owner in range(2):
                client['threads'].append({'owner':owner,'overflow':False,'stopped_ns':4000000100,
                    'connections':[{'life':position+1,'ready_ns':1,'closed':True,'end_reason':2,
                                    'pending_at_T1':False,'sequence':4,'start_ns':3000000000,
                                    'sent_bytes':len(request),'full_sent':True,'active':False,
                                    'wait_lower_bound_ns':0} for position in range(64)],
                    'slow_capacity':16384,'slow_records':[],'pending_at_T1':0,'pending_warm':0,
                    'censored_by_start_phase':[[0,0,0],[0,0,0]],'censored_unsent':0,'censored_partial':0,
                    'censored_sent_deadline_after_T1':0,
                    'errors':{name:0 for name in ('connect','read','write','status','timeout','headers','body','protocol')},
                    'completed':[0,128,128,0]})
            (root/'client.json').write_text(json.dumps(client))
            active_build = {'serverSourceCommit':'acda3f92d42a36d0b0554e185bc6f4155b4e5889',
                            'serverSha256':hashlib.sha256(b'hand-server').hexdigest(),
                            'clientSha256':hashlib.sha256(b'hand-client').hexdigest(),
                            'packageRoot':str(package),'outputRoot':str(root/'hand-build'),
                            'actualLoadedLibraries':{str(root/'hand-build'/suffix):{'files':dependencies}
                                                     for suffix in ('build-E/hp_http_server','client/wrk-baseline')}}
            (root/'build-receipt.json').write_text(json.dumps({'schema':'baseline-v1-build','builds':[active_build,active_build]}))
            (root/'document-root').mkdir()
            (root/'document-root/payload-1024.bin').write_bytes(bytes(range(256))*4)
            sample = {'schema':'baseline-v1','status':'valid','first_error':None,'role':'builder',
                      'run_id':'run-r015-smoke-001','source_commit':'acda3f92d42a36d0b0554e185bc6f4155b4e5889',
                      'package_sha256':hashlib.sha256((package/'inputs-lock.json').read_bytes()).hexdigest(),'cleanup_errors':[],
                      'cleanup':[{'reaped':True,'unknown':[],'errors':[]} for _ in range(2)],'catalog':[]}
            sample.update(uid=1000,gid=1000,runner_identity={'pid':12344,'starttime':98},
                          build_receipt_sha256=hashlib.sha256((root/'build-receipt.json').read_bytes()).hexdigest(),
                          server_sha256=active_build['serverSha256'],client_sha256=active_build['clientSha256'],
                          payload_sha256=hashlib.sha256(bytes(range(256))*4).hexdigest(),
                          request_bytes=len(request),request_sha256=client['request_sha256'],
                          environment={'LD_LIBRARY_PATH':client['actual_LD_LIBRARY_PATH'],'LUA_PATH':client['actual_LUA_PATH']},
                          runtime_dependencies=dependencies,processes=[],resources=[])
            for position,name in enumerate(('server','client')):
                identity={'pid':12345+position,'starttime':99+position}
                sample['processes'].append({'name':name,'identity':identity})
                snapshots=[dict(identity,observed_ns=100+index*1000000000,utime_ticks=10+index*10,
                                stime_ticks=2,clock_ticks_per_second=100,rss_bytes=4096) for index in range(2)]
                sample['resources'].append({'name':name,'identity':identity,
                    'scope':'sampled_process_envelope_not_measurement_only_or_final_CPU',
                    'samples':snapshots,'cpu_percent':10.0,'sampled_rss_max_bytes':4096})
            for name in (*verifier.NAMES, 'client.json','build-receipt.json','document-root/payload-1024.bin'):
                data = (root/name).read_bytes()
                sample['catalog'].append({'path':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
            (root/'sample.json').write_text(json.dumps(sample))
            return root

        def change_client_fixture(self, root, change):
            client_path = root/'client.json'
            client = json.loads(client_path.read_text())
            change(client)
            client_path.write_text(json.dumps(client))
            sample_path = root/'sample.json'
            sample = json.loads(sample_path.read_text())
            data = client_path.read_bytes()
            row = next(row for row in sample['catalog'] if row['path']=='client.json')
            row.update(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
            sample_path.write_text(json.dumps(sample))

        def make_fixture(self):
            root = Path(tempfile.mkdtemp(prefix='package-fixture-', dir=scratch))
            source = root / 'input.bin'
            source.write_bytes(b'fixed-input')
            lock = {'schema':'baseline-v1-input-lock',
                    'serverCommit':'acda3f92d42a36d0b0554e185bc6f4155b4e5889',
                    'files': [{'path': 'input.bin', 'bytes': 11,'source':'hand fixture',
                               'license':'project local fixture','required':True,
                               'sha256': hashlib.sha256(b'fixed-input').hexdigest()}]}
            (root / 'inputs-lock.json').write_text(json.dumps(lock))
            return root

        def test_actual_package_closure(self):
            prepare.verify_package(package)

        def test_missing_input_rejected(self):
            root = self.make_fixture()
            (root / 'input.bin').unlink()
            with self.assertRaises(ValueError):
                prepare.verify_package(root)

        def test_drift_rejected(self):
            root = self.make_fixture()
            (root / 'input.bin').write_bytes(b'fixed-inpuT')
            with self.assertRaises(ValueError):
                prepare.verify_package(root)

        def test_undeclared_input_rejected(self):
            root = self.make_fixture()
            (root / 'profile.lua').write_bytes(b'undeclared')
            with self.assertRaises(ValueError):
                prepare.verify_package(root)

        def test_missing_license_identity_rejected(self):
            root = self.make_fixture()
            path = root/'inputs-lock.json'
            lock = json.loads(path.read_text())
            del lock['files'][0]['license']
            path.write_text(json.dumps(lock))
            with self.assertRaises(ValueError):
                prepare.verify_package(root)

        def test_duplicate_and_escape_lock_rejected(self):
            root = self.make_fixture()
            lock_path = root / 'inputs-lock.json'
            lock = json.loads(lock_path.read_text())
            lock['files'] += lock['files']
            lock_path.write_text(json.dumps(lock))
            with self.assertRaises(ValueError):
                prepare.verify_package(root)
            lock['files'] = [{'path': '../old-cache/input.bin', 'bytes': 1, 'sha256': '0'*64}]
            lock_path.write_text(json.dumps(lock))
            with self.assertRaises(ValueError):
                prepare.verify_package(root)

        def test_link_refused_before_old_cache_read(self):
            root = self.make_fixture()
            real_lstat = Path.lstat
            def link_metadata(path):
                return SimpleNamespace(st_mode=stat.S_IFLNK) if path == root/'input.bin' else real_lstat(path)
            with mock.patch.object(Path, 'lstat', link_metadata), \
                 mock.patch.object(Path, 'read_bytes', side_effect=AssertionError('old workspace accessed')):
                with self.assertRaises(ValueError):
                    prepare.verify_package(root)
            def root_link_metadata(path):
                return SimpleNamespace(st_mode=stat.S_IFLNK) if path == root else real_lstat(path)
            with mock.patch.object(Path, 'lstat', root_link_metadata), self.assertRaises(ValueError):
                prepare.verify_package(root)

        def test_linked_parent_refused(self):
            root = self.make_fixture()
            real_lstat = Path.lstat
            def parent_link_metadata(path):
                return SimpleNamespace(st_mode=stat.S_IFLNK) if path == root.parent else real_lstat(path)
            with mock.patch.object(Path,'lstat',parent_link_metadata), self.assertRaises(ValueError):
                prepare.verify_package(root)

        def test_actual_bin_decoder(self):
            self.assertEqual(verifier.decode_bins(struct.pack('<QQQQ', 0, 2, 2000000, 1)), {0: 2, 2000000: 1})
            bad = [b'\x00', struct.pack('<QQ', 1, 0), struct.pack('<QQ', 2000001, 1),
                   struct.pack('<QQQQ', 5, 1, 5, 2), struct.pack('<QQQQ', 4, 1, 2, 1),
                   struct.pack('<QQQQ', 0, (1<<64)-1, 1, 1)]
            for value in bad:
                with self.subTest(bytes=len(value)), self.assertRaises(ValueError):
                    verifier.decode_bins(value)

        def test_hand_calculated_original_correction(self):
            self.assertEqual(verifier.reconstruct_correction({4000: 1}, 1000), {4000: 1, 3000: 1, 2000: 1})
            bins = verifier.reconstruct_correction({500: 1, 4000: 1}, 1000)
            self.assertEqual(bins, {500: 1, 4000: 1, 3000: 1, 2000: 1})
            self.assertEqual(verifier.original_percentile(bins, 500, 50), 3000)
            self.assertEqual(verifier.original_percentile(bins, 500, 99), 4000)
            self.assertEqual(verifier.original_percentile({2000: 1, 3000: 1, 4000: 1}, 4000, 99), 0)
            self.assertEqual(verifier.reconstruct_correction({100: 128}, 3000000), {100: 128})
            for interval in (0, -1, 1.5):
                with self.assertRaises(ValueError):
                    verifier.reconstruct_correction({500: 1}, interval)

        def test_regular_reader_rejects_symlink(self):
            root = self.make_fixture()
            with mock.patch.object(verifier.os,'open',side_effect=OSError('nofollow rejected link')) as opened:
                with self.assertRaises(OSError):
                    verifier.read_regular(root/'input.bin',100)
                self.assertTrue(opened.call_args.args[1] & verifier.os.O_NOFOLLOW)
            with self.assertRaises(ValueError):
                verifier.read_regular(root / 'input.bin', 1)

        def test_missing_sample_and_histograms_rejected(self):
            root = self.make_fixture()
            with self.assertRaises((OSError, ValueError, KeyError)):
                verifier.verify_sample(root)
            (root / 'sample.json').write_text(json.dumps({'schema':'baseline-v1','status':'invalid'}))
            with self.assertRaises(ValueError):
                verifier.verify_sample(root)

        def test_actual_full_sample_contract(self):
            self.assertEqual(verifier.verify_sample(self.make_sample_fixture())['N'],512)

        def test_missing_bucket_corruption_and_duplicate_catalog(self):
            for kind in ('missing','corrupted','duplicate'):
                root = self.make_sample_fixture()
                sample_path = root/'sample.json'
                sample = json.loads(sample_path.read_text())
                if kind == 'missing':
                    sample['catalog'] = [row for row in sample['catalog'] if row['path']!='main-raw.bin']
                elif kind == 'duplicate':
                    sample['catalog'].append(sample['catalog'][0])
                else:
                    (root/'main-raw.bin').write_bytes(b'bad')
                sample_path.write_text(json.dumps(sample))
                with self.subTest(kind=kind), self.assertRaises(ValueError):
                    verifier.verify_sample(root)

        def test_units_duplicate_count_and_lost_pending_rejected(self):
            changes = [lambda client: client.update(duration_ns=3000000),
                       lambda client: client.update(N=513),
                       lambda client: client['threads'][0].update(pending_at_T1=1),
                       lambda client: client['threads'][0].update(completed=[0,129,128,0]),
                       lambda client: client['threads'][0].update(censored_unsent=1),
                       lambda client: client['threads'][0].update(censored_by_start_phase=[[1,0,0],[0,0,0]]),
                       lambda client: client['threads'][0]['errors'].update(read=1),
                       lambda client: client['threads'][0]['slow_records'].append({'life':1,'sequence':5,
                           'start_ns':3000000000,'complete_ns':3050000000,'status':200,'body_bytes':1024,'class':2})]
            for change in changes:
                root = self.make_sample_fixture()
                self.change_client_fixture(root,change)
                with self.assertRaises(ValueError):
                    verifier.verify_sample(root)

        def test_owned_cleanup_exact_identity(self):
            process = mock.Mock(pid=12345, returncode=0)
            process.poll.side_effect = [None, None, 0, 0]
            process.wait.return_value = 0
            owner = owned.OwnedProcess(process, {'pid':12345,'starttime':99})
            with mock.patch.object(owned, 'process_identity', return_value={'pid':12345,'starttime':99}), \
                 mock.patch.object(owned.os, 'kill') as kill:
                result = owner.close(owned.time.monotonic() + 1)
                self.assertTrue(result['reaped'])
                self.assertEqual(result['errors'], [])
                self.assertEqual(result['unknown'], [])
                kill.assert_called_once_with(12345, owned.signal.SIGTERM)

        def test_reused_identity_never_signalled(self):
            process = mock.Mock(pid=12345, returncode=0)
            process.poll.side_effect = [None, None, 0, 0]
            owner = owned.OwnedProcess(process, {'pid':12345,'starttime':99})
            with mock.patch.object(owned, 'process_identity', return_value={'pid':12345,'starttime':100}), \
                 mock.patch.object(owned.os, 'kill') as kill:
                result = owner.close(owned.time.monotonic() + 1)
                kill.assert_not_called()
                self.assertTrue(result['errors'])
                self.assertTrue(result['unknown'])

        def test_first_cleanup_failure_preserved(self):
            process = mock.Mock(pid=12345, returncode=None)
            process.poll.return_value = None
            process.wait.side_effect = subprocess.TimeoutExpired('owned', 0.1)
            owner = owned.OwnedProcess(process, {'pid':12345,'starttime':99})
            with mock.patch.object(owned, 'process_identity', return_value={'pid':12345,'starttime':99}), \
                 mock.patch.object(owned.os, 'kill'):
                result = owner.close(owned.time.monotonic() + 1)
                self.assertFalse(result['reaped'])
                self.assertTrue(result['unknown'])
                self.assertEqual(result['errors'][0]['operation'], 'wait_after_TERM')

        def test_expired_cleanup_never_signals(self):
            process = mock.Mock(pid=12345, returncode=None)
            process.poll.return_value = None
            owner = owned.OwnedProcess(process,{'pid':12345,'starttime':99})
            with mock.patch.object(owned.os,'kill') as kill:
                result = owner.close(owned.time.monotonic()-1)
                kill.assert_not_called()
                process.wait.assert_not_called()
                self.assertFalse(result['reaped'])
                self.assertEqual(result['unknown'][0]['reason'],'deadline_exhausted')

    return unittest.defaultTestLoader.loadTestsFromTestCase(PackageTests)
