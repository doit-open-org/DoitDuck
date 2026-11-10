#!/usr/bin/env python3
"""Import a policy (an ONNX file and its manifest.json) into a running DoitDuck.

The ONNX graph must be (obs - mean) / std -> Gemm -> Elu -> Gemm -> Elu ->
Gemm -> Elu -> Gemm, 61 -> 512 -> 256 -> 128 -> 14, as every Microduck and
DoitDuck policy is. It is converted here to the float32 file the robot runs
(little endian: mean[61], std[61], then weight[out][in] and bias[out] of each
layer) and sent to POST /api/policy.import, the
same request the web page makes. Only the Python standard library is needed.

    python3 tools/import_policy.py policy.onnx manifest.json --host 192.168.137.2
    python3 tools/import_policy.py policy.onnx manifest.json --out kick.bin   # convert only
"""
import argparse
import base64
import json
import math
import struct
import sys
import urllib.error
import urllib.request

SHAPES = [(61,), (61,), (512, 61), (512,), (256, 512), (256,), (128, 256), (128,), (14, 128), (14,)]


def varint(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, i


def fields(b):
    """(field number, wire type, value) of a protobuf message."""
    i = 0
    while i < len(b):
        k, i = varint(b, i)
        f, w = k >> 3, k & 7
        if w == 0:
            v, i = varint(b, i)
        elif w == 1:
            v, i = b[i:i + 8], i + 8
        elif w == 2:
            n, i = varint(b, i)
            v, i = b[i:i + n], i + n
        elif w == 5:
            v, i = b[i:i + 4], i + 4
        else:
            raise ValueError(f'protobuf wire type {w}')
        yield f, w, v


def tensor(b):
    """name, dims and float32 bytes (little endian) of a TensorProto."""
    name, dims, dtype, raw, floats = '', [], 1, None, []
    for f, w, v in fields(b):
        if f == 1 and w == 0:
            dims.append(v)
        elif f == 1:
            j = 0
            while j < len(v):
                x, j = varint(v, j)
                dims.append(x)
        elif f == 2:
            dtype = v
        elif f == 4 and w == 5:
            floats.append(v)
        elif f == 4:
            floats += [v[j:j + 4] for j in range(0, len(v), 4)]
        elif f == 8:
            name = v.decode()
        elif f == 9:
            raw = bytes(v)
        elif f == 14:
            sys.exit(f'{name}: external tensor data is not supported')
    if dtype != 1:
        sys.exit(f'{name}: not float32 (data type {dtype})')
    return name, dims, raw if raw is not None else b''.join(floats)


def convert(onnx):
    """The robot's weight file from an ONNX policy; exits with the reason when the graph is another."""
    graph = next(v for f, w, v in fields(onnx) if f == 7)
    nodes, init = [], {}
    for f, w, v in fields(graph):
        if f == 1:
            node = {'in': [], 'attr': {}}
            for nf, nw, nv in fields(v):
                if nf == 1:
                    node['in'].append(nv.decode())
                elif nf == 4:
                    node['op'] = nv.decode()
                elif nf == 5:
                    a = {af: (av if aw == 0 else struct.unpack('<f', av)[0] if aw == 5 else bytes(av))
                         for af, aw, av in fields(nv)}
                    node['attr'][a[1].decode()] = a.get(2, a.get(3))
            nodes.append(node)
        elif f == 5:
            name, dims, data = tensor(v)
            init[name] = (dims, data)
    ops = [n['op'] for n in nodes]
    if ops != ['Sub', 'Div'] + ['Gemm', 'Elu'] * 3 + ['Gemm']:
        sys.exit(f'unexpected graph: {ops}')
    for n in nodes:
        a = n['attr']
        if n['op'] == 'Gemm' and (a.get('transB') != 1 or a.get('alpha', 1.0) != 1.0 or a.get('beta', 1.0) != 1.0
                                  or a.get('transA', 0) != 0):
            sys.exit(f'unexpected Gemm attributes: {a}')
        if n['op'] == 'Elu' and a.get('alpha', 1.0) != 1.0:
            sys.exit(f'unexpected Elu alpha: {a}')
    names = [nodes[0]['in'][1], nodes[1]['in'][1]] + [x for n in nodes if n['op'] == 'Gemm' for x in n['in'][1:3]]
    out = b''
    for name, shape in zip(names, SHAPES):
        if name not in init:
            sys.exit(f'{name} is not a constant of the graph')
        dims, data = init[name]
        if dims[:1] == [1] and len(dims) == len(shape) + 1:
            dims = dims[1:]  # mean and std may be [1, 61]
        if tuple(dims) != shape or len(data) != 4 * math.prod(shape):
            sys.exit(f'{name}: shape {init[name][0]}, expected {shape}')
        out += data
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('onnx')
    ap.add_argument('manifest')
    ap.add_argument('--host', help='the robot, e.g. 192.168.137.2 or localhost')
    ap.add_argument('--port', type=int, default=80)
    ap.add_argument('--name', help='name on the robot; the manifest\'s by default')
    ap.add_argument('--confirm', action='store_true', help='allow replacing the walk or get-up policy')
    ap.add_argument('--out', help='only write the converted weights to this file')
    args = ap.parse_args()

    weights = convert(open(args.onnx, 'rb').read())
    manifest = json.load(open(args.manifest, encoding='utf-8'))
    if args.out:
        open(args.out, 'wb').write(weights)
        print(f'{args.onnx} -> {args.out}')
        return
    if not args.host:
        ap.error('--host or --out is needed')
    body = {'manifest': manifest, 'weights': base64.b64encode(weights).decode(), 'confirm': args.confirm}
    if args.name:
        body['name'] = args.name
    req = urllib.request.Request(f'http://{args.host}:{args.port}/api/policy.import', json.dumps(body).encode(),
                                 {'Content-Type': 'application/json'})
    try:
        reply = json.load(urllib.request.urlopen(req, timeout=30))
    except urllib.error.HTTPError as e:
        reply = json.load(e)
    if 'error' in reply:
        sys.exit(f"refused: {reply['error']['code']}: {reply['error']['message']}")
    r = reply['result']
    print(f"imported {r['name']}" + (f", replacing {r['replaced']}" if 'replaced' in r else '') +
          '; try it in preview mode, then mark it verified on the web page')


if __name__ == '__main__':
    main()
