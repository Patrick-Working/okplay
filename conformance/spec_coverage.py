"""Coverage of the DirectPlay 4 wire protocol ([MC-DPL4CS] rev 20.0, section 2.2.6) by our Wine-based dplayx.

Clean-room rule: the only inputs are Microsoft's published Open Specification (whose licence allows building
implementations from it), Wine's source, and black-box observation of Windows. Never Microsoft's binaries.

Reads the DPMSGCMD_* defines and their uses in winedplay/port/dlls/dplayx and prints, per spec command,
whether our code sends it, whether DP_HandleMessage handles it, and whether MTM2 needs it.

  python spec_coverage.py            # markdown table to stdout
  python spec_coverage.py --csv      # CSV
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'winedplay', 'port', 'dlls', 'dplayx')

# value: (spec name, spec section, needed by MTM2 (DirectPlay2A, TCP/IP, flags 0x42/0x46, no groups, no security))
SPEC = {
    0x01: ('ENUMSESSIONSREPLY', '2.2.30', 'yes'),
    0x02: ('ENUMSESSIONS', '2.2.29', 'yes'),
    0x03: ('ENUMPLAYERSREPLY', '2.2.28', 'yes'),
    0x04: ('ENUMPLAYER', '2.2.27', 'no'),
    0x05: ('REQUESTPLAYERID', '2.2.49', 'yes'),
    0x06: ('REQUESTGROUPID', '2.2.48', 'no'),
    0x07: ('REQUESTPLAYERREPLY', '2.2.50', 'yes'),
    0x08: ('CREATEPLAYER', '2.2.21', 'yes'),
    0x09: ('CREATEGROUP', '2.2.20', 'no'),
    0x0A: ('PLAYERMESSAGE', '2.2.45', 'check'),
    0x0B: ('DELETEPLAYER', '2.2.25', 'yes'),
    0x0C: ('DELETEGROUP', '2.2.23', 'no'),
    0x0D: ('ADDPLAYERTOGROUP', '2.2.12', 'no'),
    0x0E: ('DELETEPLAYERFROMGROUP', '2.2.26', 'no'),
    0x0F: ('PLAYERDATACHANGED', '2.2.44', 'no'),
    0x10: ('PLAYERNAMECHANGED', '2.2.46', 'no'),
    0x11: ('GROUPDATACHANGED', '2.2.31', 'no'),
    0x12: ('GROUPNAMECHANGED', '2.2.32', 'no'),
    0x13: ('ADDFORWARDREQUEST', '2.2.11', 'yes'),
    0x15: ('PACKET', '2.2.39', 'check'),
    0x16: ('PING', '2.2.42', 'yes'),
    0x17: ('PINGREPLY', '2.2.43', 'yes'),
    0x18: ('YOUAREDEAD', '2.2.55', 'yes'),
    0x19: ('PLAYERWRAPPER', '2.2.47', 'check'),
    0x1A: ('SESSIONDESCCHANGED', '2.2.51', 'yes'),
    0x1C: ('CHALLENGE', '2.2.17', 'no'),
    0x1D: ('ACCESSGRANTED', '2.2.7', 'no'),
    0x1E: ('LOGONDENIED', '2.2.36', 'no'),
    0x1F: ('AUTHERROR', '2.2.16', 'no'),
    0x20: ('NEGOTIATE', '2.2.38', 'no'),
    0x21: ('CHALLENGERESPONSE', '2.2.18', 'no'),
    0x22: ('SIGNED', '2.2.52', 'no'),
    0x24: ('ADDFORWARDREPLY', '2.2.10', 'yes'),
    0x25: ('ASK4MULTICAST', '2.2.14', 'no'),
    0x26: ('ASK4MULTICASTGUARANTEED', '2.2.15', 'no'),
    0x27: ('ADDSHORTCUTTOGROUP', '2.2.13', 'no'),
    0x28: ('DELETEGROUPFROMGROUP', '2.2.24', 'no'),
    0x29: ('SUPERENUMPLAYERSREPLY', '2.2.53', 'yes'),
    0x2B: ('KEYEXCHANGE', '2.2.34', 'no'),
    0x2C: ('KEYEXCHANGEREPLY', '2.2.35', 'no'),
    0x2D: ('CHAT', '2.2.19', 'no'),
    0x2E: ('ADDFORWARD', '2.2.8', 'yes'),
    0x2F: ('ADDFORWARDACK', '2.2.9', 'yes'),
    0x30: ('PACKET2_DATA', '2.2.41', 'no'),
    0x31: ('PACKET2_ACK', '2.2.40', 'no'),
    0x35: ('IAMNAMESERVER', '2.2.33', 'yes'),
    0x36: ('VOICE', '2.2.54', 'no'),
    0x37: ('MULTICASTDELIVERY', '2.2.37', 'no'),
    0x38: ('CREATEPLAYERVERIFY', '2.2.22', 'no'),
}


def read(name):
    with open(os.path.join(SRC, name), encoding='latin-1') as f:
        return f.read()


def main():
    defines = {}
    for name in ('dplayx_messages.h', 'dplay_global.h'):
        for m in re.finditer(r'#define\s+DPMSGCMD_(\w+)\s+(\d+)', read(name)):
            defines[m.group(1)] = int(m.group(2))
    sends, handles = {}, set()
    for fn in sorted(os.listdir(SRC)):
        if not fn.endswith('.c'):
            continue
        for ln, line in enumerate(read(fn).splitlines(), 1):
            m = re.search(r'wCommandId\s*=\s*DPMSGCMD_(\w+)', line)
            if m:
                sends.setdefault(m.group(1), []).append('%s:%d' % (fn, ln))
            m = re.match(r'\s*case\s+DPMSGCMD_(\w+)\s*:', line)
            if m and fn == 'dplay.c':
                handles.add(m.group(1))
    by_value = {}
    for wname, v in defines.items():
        by_value.setdefault(v, []).append(wname)

    rows = []
    for v in sorted(set(SPEC) | set(by_value)):
        sname, sec, mtm2 = SPEC.get(v, ('(not in spec)', '', ''))
        wnames = by_value.get(v, [])
        snd = [s for w in wnames for s in sends.get(w, [])]
        rcv = any(w in handles for w in wnames)
        rows.append(dict(value='0x%02X' % v, spec=sname, section=sec, wine=' '.join(wnames) or '-',
                         sends='yes' if snd else 'no', handles='yes' if rcv else 'no', mtm2=mtm2,
                         where=' '.join(snd[:2])))

    if '--csv' in sys.argv:
        keys = list(rows[0])
        print(','.join(keys))
        for r in rows:
            print(','.join(r[k] for k in keys))
        return
    print('| cmd | spec name | spec | Wine name | sends | handles | MTM2 | sent at |')
    print('|---|---|---|---|---|---|---|---|')
    for r in rows:
        print('| %(value)s | %(spec)s | %(section)s | %(wine)s | %(sends)s | %(handles)s | %(mtm2)s | %(where)s |' % r)
    spec_rows = [r for r in rows if r['section']]
    full = sum(r['sends'] == 'yes' or r['handles'] == 'yes' for r in spec_rows)
    need = [r for r in spec_rows if r['mtm2'] == 'yes']
    need_ok = sum(r['sends'] == 'yes' or r['handles'] == 'yes' for r in need)
    print('\nspec commands touched by our code: %d of %d; MTM2-needed commands touched: %d of %d'
          % (full, len(spec_rows), need_ok, len(need)))


if __name__ == '__main__':
    main()
