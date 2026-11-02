#!/usr/bin/env python3
"""Exercise Merkle terrain exchange between two real engine clients and a server.
Uses isolated assets and existing SDL command injection; no user configs touched.
"""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import subprocess
import tempfile
import threading
import struct
import time
from types import SimpleNamespace
from run_terrain_lod_smoke import Run, WRAPPER
from run_world_network_smoke import Client, control, copied_engine_environment, WORLD_PROTOCOL_VERSION


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--require-nat', action='store_true', help='Require transfer through GameNetworkingSockets, not TCP fallback')
    args = parser.parse_args()
    os.environ.update(copied_engine_environment(args.binary))
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    work = Path(tempfile.mkdtemp(prefix='quiver-peer-smoke-'))
    binary = work / 'vkquake'
    shutil.copy2(args.binary.resolve(), binary)
    (work / 'autokey.c').write_text(WRAPPER)
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', str(work / 'autokey.c'), '-o', str(work / 'autokey.so'), '-ldl'], check=True)
    def assets(path):
        (path / 'id1').mkdir(parents=True)
        (path / 'user').mkdir()
        for asset in source.glob('pak*.pak'): (path / 'id1' / asset.name).symlink_to(asset)
        if (source / 'progs.dat').exists(): (path / 'id1/progs.dat').symlink_to(source / 'progs.dat')
        (path / 'vkquake').symlink_to(binary)
        (path / 'autokey.so').symlink_to(work / 'autokey.so')
    serverdir = work / 'server'; assets(serverdir)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
    serverlog = (serverdir / 'process.log').open('wb')
    server = subprocess.Popen([str(binary), '-basedir', str(serverdir), '-userdir', str(serverdir / 'user'), '-dedicated', '4',
        '-port', str(port), '+developer', '1', '+deathmatch', '0', '+coop', '0', '+map', '__octree'],
        stdin=subprocess.PIPE, stdout=serverlog, stderr=subprocess.STDOUT)
    clients=[]; selector=selectors.DefaultSelector(); actor=None
    result={'success':False, 'directory':str(work)}
    try:
        started=time.monotonic()
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
            probe.settimeout(.05)
            while time.monotonic()-started<30:
                probe.sendto(control(b'\x02QUAKE\x00\x03'),('127.0.0.1',port))
                try:
                    reply,_=probe.recvfrom(4096)
                    if b'__octree\x00' in reply: break
                except socket.timeout: pass
            else: raise RuntimeError('server readiness timeout')
        actor=Client(0,('127.0.0.1',port),selector,godmode=True)
        actor.drop_first_chunk_ack = True
        edited=False; edit_at=None
        while time.monotonic()-started<45:
            now=time.monotonic(); actor.pump(now,started,False,False)
            for key,_ in selector.select(.01):
                data,address=key.fileobj.recvfrom(65536); actor.receive(data,address,now,started)
            if actor.frames and actor.godmode_confirmed and not edited:
                server.stdin.write(b'world_teleport 0 0 0 128 128 88\n'); server.stdin.flush()
                edited=True; edit_at=now
            if edit_at and now-edit_at>.5:
                actor.edit(1,0,60,x=164); edit_at=None
            if actor.edit_replies and actor.ack_retries: break
        else: raise RuntimeError('fixture excavation not committed')
        def start_client(index, local=False):
            path=work / f'client{index}'; assets(path)
            connection = "map __octree" if local else f"connect 127.0.0.1:{port}"
            (path / 'id1/startup.cfg').write_text(f'''developer 1
vid_fullscreen 0
vid_vsync 0
vid_fsaa 0
host_maxfps 60
r_clouds 0
r_atmosphere 0
r_terrain_lod 0
bind F12 "exec smoke_command.cfg"
name peer{index}
{connection}
''')
            run=Run(SimpleNamespace(timeout=150,stage_timeout=30,width=640,height=480),path)
            clients.append(run)
            run.wait(lambda text:'CL_SignonReply: 4' in text,'client connected')
            return run
        def stats(run):
            text=run.command('world_stats')
            m=re.search(r'World peers: served (\d+) fetched (\d+) uploaded (\d+) downloaded (\d+) cached (\d+)',text)
            if not m: raise RuntimeError('missing peer counters: '+text)
            values=dict(zip(('served','fetched','uploaded','downloaded','cached'),map(int,m.groups())))
            nat=re.search(r'World peer NAT: connected (\d+) served (\d+) fetched (\d+) failed (\d+)',text)
            if not nat: raise RuntimeError('missing NAT counters: '+text)
            values.update(zip(('nat_connected','nat_served','nat_fetched','nat_failed'),map(int,nat.groups())))
            return values
        first=start_client(1)
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            a=stats(first)
            if a['cached']: break
            first.delay(.2)
        else: raise RuntimeError('first client did not cache verified override')
        second=start_client(2)
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            a,b=stats(first),stats(second)
            if a['served'] and b['fetched'] and b['cached'] and (not args.require_nat or (a['nat_served'] and b['nat_fetched'])): break
            second.delay(.2)
        else: raise RuntimeError(f'peer transfer not observed: {a} {b}')
        result.update(success=True,provider=a,receiver=b,nat_required=args.require_nat,fixture_edit=[entry[:3] for entry in actor.edit_replies], dropped_application_ack_retried=actor.ack_retries)
        if args.require_nat:
            # Keep the provider's game session alive while disabling and
            # re-enabling NAT. Receiver reconnects discard its verified cache,
            # forcing fresh object requests through the provider each time.
            provider_id=int(re.search(r'World peer NAT session (\d+): configured',first.text()).group(1))
            def directory_capability(expected):
                deadline=time.monotonic()+8
                while time.monotonic()<deadline:
                    now=time.monotonic(); actor.pump(now,started,False,False)
                    for key,_ in selector.select(.01):
                        data,address=key.fileobj.recvfrom(65536); actor.receive(data,address,now,started)
                    entry=actor.peer_directory.get(provider_id)
                    if entry and entry[0] == expected:
                        if not entry[1]: raise RuntimeError('NAT toggle lost TCP listener advertisement')
                        return entry
                raise RuntimeError(f'provider NAT capability did not change to {expected}: {actor.peer_directory}')
            def reconnect_receiver():
                previous=second.text().count('CL_SignonReply: 4')
                second.command('disconnect')
                second.command(f'connect 127.0.0.1:{port}')
                second.wait(lambda text:text.count('CL_SignonReply: 4')>previous,'receiver reconnected')
            first.command('world_peer_nat 0')
            disabled_directory=directory_capability(False)
            a=stats(first)
            if any(a[key] for key in ('nat_connected','nat_served','nat_fetched','nat_failed')):
                raise RuntimeError(f'disabled NAT transport remained active: {a}')
            reconnect_receiver()
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                a,b=stats(first),stats(second)
                if a['served'] and b['fetched'] and b['cached']: break
                second.delay(.2)
            else: raise RuntimeError(f'TCP fallback while NAT disabled not observed: {a} {b}')
            if a['nat_connected'] or a['nat_served'] or b['nat_connected'] or b['nat_fetched']:
                raise RuntimeError(f'disabled provider still used NAT: {a} {b}')
            result['nat_disabled_tcp_fallback']={'provider':a,'receiver':b,'directory':disabled_directory}
            configured=first.text().count(f'World peer NAT session {provider_id}: configured')
            first.command('world_peer_nat 1')
            enabled_directory=directory_capability(True)
            first.wait(lambda text:text.count(f'World peer NAT session {provider_id}: configured')>configured,'same-session NAT reconfiguration')
            reconnect_receiver()
            deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                a,b=stats(first),stats(second)
                if a['nat_served'] and b['nat_fetched'] and b['cached']: break
                second.delay(.2)
            else: raise RuntimeError(f'NAT transfer after re-enable not observed: {a} {b}')
            result['nat_reenabled_same_session']={'provider':a,'receiver':b,'directory':enabled_directory,'provider_id':provider_id}
        # A departed peer must never prevent subsequent server delivery.
        first.command('disconnect')
        second.command('disconnect')
        second.command(f'connect 127.0.0.1:{port}')
        second.delay(2)
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            b=stats(second)
            if b['cached']: break
            second.delay(.2)
        else: raise RuntimeError('server fallback after peer departure failed')
        result['server_fallback_after_departure']=b
        # Advertise a session peer that returns a payload with the requested
        # hash but corrupt bytes. The engine must reject it and use the server.
        liar=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
        liar.bind(('127.0.0.1',0)); liar.listen(1); liar.settimeout(15)
        liar_port=liar.getsockname()[1]
        corrupt_seen=[]
        def corrupt_peer():
            try:
                connection,_=liar.accept()
                with connection:
                    connection.settimeout(5); request=b''
                    while len(request)<68:
                        part=connection.recv(68-len(request))
                        if not part: return
                        request+=part
                    payload=struct.pack('<IHHB',0x51565231,WORLD_PROTOCOL_VERSION,7,1)+request[36:68]+struct.pack('<I',1)+b'\xff'
                    connection.sendall(struct.pack('<I',len(payload))+payload)
                    corrupt_seen.append(True)
            finally: liar.close()
        thread=threading.Thread(target=corrupt_peer,daemon=True); thread.start()
        actor.queue.append(b'\x57'+struct.pack('<H',3)+b'\xa1'+struct.pack('<H',liar_port))
        until=time.monotonic()+1
        while time.monotonic()<until:
            now=time.monotonic(); actor.pump(now,started,False,False)
            for key,_ in selector.select(.01):
                data,address=key.fileobj.recvfrom(65536); actor.receive(data,address,now,started)
        second.command('disconnect')
        second.command(f'connect 127.0.0.1:{port}')
        second.delay(.5)
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            b=stats(second)
            if corrupt_seen and b['cached'] and 'Rejected peer terrain object' in second.text(): break
            second.delay(.1)
        else: raise RuntimeError('corrupt peer rejection/server fallback not observed')
        result['corrupt_peer_rejected_with_server_fallback']=b
        thread.join(timeout=1)
        host=start_client(3,local=True)
        host.delay(3.2)
        host_stats=host.command('world_stats')
        if 'World terrain transfers: active 0, awaiting ack 0' not in host_stats:
            raise RuntimeError('listen host redundantly streaming terrain to itself: '+host_stats)
        result['listen_host_uses_shared_terrain_without_transfers']=True
    except Exception as error:
        result.update(success=False,error=str(error))
    finally:
        for run in clients: run.close()
        if actor: actor.socket.close()
        selector.close()
        if server.poll() is None:
            server.stdin.write(b'quit\n'); server.stdin.flush()
            try: server.wait(timeout=10)
            except subprocess.TimeoutExpired: server.kill(); server.wait()
        serverlog.close()
        (work / 'summary.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2),flush=True)
    return 0 if result['success'] else 1
if __name__=='__main__': raise SystemExit(main())
