#!/usr/bin/env python3
"""Optional read-only Internet samples; downloaded artwork stays outside Git."""
import hashlib
import pathlib
import subprocess
import sys
import urllib.parse
import urllib.request


def main():
    renderer, directory = sys.argv[1:3]
    output = pathlib.Path(directory)
    output.mkdir(parents=True, exist_ok=True)
    names = ['BBC Radio Logo 2020.svg', 'Deutsche Welle Logo.svg', 'BBC Radio logo.svg']
    for index, name in enumerate(names):
        filename = name.replace(' ', '_')
        digest = hashlib.md5(filename.encode()).hexdigest()
        url = 'https://upload.wikimedia.org/wikipedia/commons/' + digest[0] + '/' + digest[:2] + '/' + urllib.parse.quote(filename)
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'StandaloneBase-SVG-Compatibility-Test/1.0'})
            with urllib.request.urlopen(req, timeout=15) as response:
                data = response.read(1024 * 1024 + 1)
            assert len(data) <= 1024 * 1024
        except Exception as error:
            print(name, 'download unavailable:', error)
            continue
        source = output / (str(index) + '.svg')
        target = output / (str(index) + '.png')
        source.write_bytes(data)
        result = subprocess.run([renderer, '--input', str(source), '--output', str(target)], capture_output=True)
        print(name, 'sha256=' + hashlib.sha256(data).hexdigest(), 'exit=' + str(result.returncode), result.stderr.decode().strip())


if __name__ == '__main__':
    main()
