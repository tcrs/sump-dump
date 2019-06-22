import collections

Signal = collections.namedtuple('Signal', 'name bits type sym')

def parse(src):
    dump_section = object()
    def value_line(l):
        if l[0] == 'b' or l[0] == 'B':
            bv, sym = l.split()
            return sym, int(bv[1:], 2)
        else:
            assert len(l) == 2, l
            return l[1], int(l[0], 2)

    def values_dict(sym_vals, sigs):
        return {sig.name: sym_vals[sig.sym] for sig in sigs}

    section = None
    contents = []
    signals = []

    timestamp = None
    current_values = {}
    for line in src:
        if section in ('dumpvars', dump_section):
            if line == '$end\n':
                section = dump_section
            else:
                if line.startswith('#'):
                    if timestamp is not None:
                        yield timestamp, values_dict(current_values, signals)
                    timestamp = int(line[1:])
                else:
                    sym, val = value_line(line.strip())
                    current_values[sym] = val
        else:
            for t in line.strip().split():
                if t == '$end':
                    if section == 'var':
                        type, bits, sym, name = contents
                        signals.append(Signal(name=name, bits=bits, type=type, sym=sym))
                    section = dump_section if section == 'dumpvars' else None
                    contents = []
                elif section in ('date', 'version', 'comment'):
                    pass
                elif t[0] == '$' and len(t) > 1: # signals may have symbol '$'
                    section = t[1:]
                else:
                    contents.append(t)
    yield timestamp, values_dict(current_values, signals)

if __name__ == '__main__':
    import sys
    for t, v in parse(sys.stdin):
        print(t, v)
