#!/usr/bin/env python3
"""Run real native merges on isolated files; preserve settings and immutable ZIPs."""
import copy
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile


def applet(name, version, **extra):
    return dict(name=name, title=name, version=version, target='baby',
                min_target_version='7.7.3', url='http://example.com/'+name+'.zip',
                sha='a'*40, **extra)


def main():
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        config, upstream = root/'config.json', root/'remote.json'
        env = os.environ | {'SBWEBSERVER_CONFIG_DIR':str(root),
                            'SB_CATALOG_CONFIG':str(config),
                            'SBBASE_PID_FILE':str(root/'absent.pid')}

        def merge(remote, success=True):
            upstream.write_text(remote if isinstance(remote,str) else json.dumps(remote))
            process = subprocess.run([executable,str(upstream)],env=env,capture_output=True)
            assert (process.returncode == 0) == success, process.stderr
            return int(process.stdout) if success else None

        original = dict(server={'name':'Keep me','host':'127.0.0.1','http_port':9010},
                        standalone_base_version='0.4.3', local_setting={'custom':True},
                        applets=[applet('StandaloneRadio','0.9.4', preferences={'theme':'local'}),
                                 applet('PrivateApplet','1.0',private_setting=17)])
        remote = dict(server={'name':'Do not import'},standalone_base_version='99.0',
                      applets=[applet('StandaloneBase','0.4.4'),applet('StandaloneRadio','0.9.6')])
        config.write_text(json.dumps(original))
        config.chmod(0o600)
        notifications=[]
        previous_handler=signal.signal(signal.SIGHUP,lambda *_:notifications.append(True))
        (root/'base.pid').write_text(str(os.getpid()))
        env['SBBASE_PID_FILE']=str(root/'base.pid')
        assert merge(remote) == 2
        assert len(notifications)==1
        actual = json.loads(config.read_text())
        assert actual['server'] == original['server']
        assert actual['standalone_base_version']=='0.4.3'
        assert actual['local_setting']==original['local_setting']
        assert actual['applets'][0]['version']=='0.9.6'
        assert actual['applets'][0]['preferences']=={'theme':'local'}
        assert actual['applets'][1]==original['applets'][1]
        assert actual['applets'][2]['name']=='StandaloneBase'
        assert config.stat().st_mode & 0o777 == 0o600
        before = config.stat(), config.read_bytes()
        assert merge(remote)==0
        assert config.stat().st_ino==before[0].st_ino
        assert config.stat().st_mtime_ns==before[0].st_mtime_ns
        assert config.read_bytes()==before[1]
        assert len(notifications)==1
        env['SBBASE_PID_FILE']=str(root/'absent.pid')
        signal.signal(signal.SIGHUP,previous_handler)

        for version in ['0.9.7','1.0','0.10.0','0.9.6-custom','0.9.6']:
            local = copy.deepcopy(original)
            local['applets']=[applet('StandaloneRadio',version)]
            config.write_text(json.dumps(local))
            assert merge({'applets':[applet('StandaloneRadio','0.9.6')]})==0
            assert json.loads(config.read_text())==local

        for pin in ('explicit','zip'):
            item=applet('StandaloneBase','0.4.3')
            if pin=='explicit': item['catalog_pin']=True
            else: item['url']='http://127.0.0.1:80/applets/packages/local.zip'
            local={'applets':[item]}
            config.write_text(json.dumps(local))
            assert merge({'applets':[applet('StandaloneBase','0.4.4')]})==0
            assert json.loads(config.read_text())==local

        config.write_text(json.dumps(original))
        before=config.read_bytes()
        invalid = ['{', '{}', '{"applets":null}', '{"applets":[]}',
                   '{"applets":[],}',
                   '{"applets":[1]}', '{"applets":[]} trailing',
                   '{"applets":[],"applets":[]}', ' '*16385,
                   {'applets':[applet('Bad','1.0'),applet('Bad','1.0')]},
                   {'applets':[applet('Bad','1.0',unexpected_setting=True)]}]
        for field,value in [('name','bad name'),('sha','bad'),('url','file:///tmp/x'),
                            ('url','http://'+'x'*600),('url','http://host/x\npath'),
                            ('url','https://user:password@host/x'),('title',''),
                            ('name','Bad\x00hidden')]:
            item=applet('Bad','1.0');item[field]=value
            invalid.append({'applets':[item]})
        for data in invalid:
            merge(data,False)
            assert config.read_bytes()==before

        full={'applets':[applet('Custom'+str(i),'1.0') for i in range(32)]}
        config.write_text(json.dumps(full));before=config.read_bytes()
        merge({'applets':[applet('StandaloneBase','0.4.4')]},False)
        assert config.read_bytes()==before

        large={'applets':[applet('Custom','1.0',padding='x'*14900)]}
        config.write_text(json.dumps(large));before=config.read_bytes()
        merge(remote,False)
        assert config.read_bytes()==before

        config.write_text('{"applets":[]}')
        assert merge({'applets':[applet('StandaloneBase','0.4.4')]})==1

        config.write_text(json.dumps({'applets':[applet('StandaloneRadio','0.9.4')]}))
        paths=[]
        for version in ['0.9.10','0.9.9']:
            path=root/(version+'.json');paths.append(path)
            path.write_text(json.dumps({'applets':[applet('StandaloneRadio',version)]}))
        jobs=[subprocess.Popen([executable,str(path)],env=env,stdout=subprocess.PIPE) for path in paths]
        assert all(job.wait()==0 for job in jobs)
        assert json.loads(config.read_text())['applets'][0]['version']=='0.9.10'
    print('catalog-merge-tests-ok')


if __name__=='__main__':
    main()
