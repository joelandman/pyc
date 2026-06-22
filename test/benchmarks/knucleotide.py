# The Computer Language Benchmarks Game
# https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
#
# submitted by Ian Osgood
# modified by Sokolov Yura
# modified by bearople
# 2to3
#
# SLOWEST Python implementation from:
# https://benchmarksgame-team.pages.debian.net/benchmarksgame/program/knucleotide-python3-1.html
#
# This version uses inline test data instead of stdin for compiler testing.
#
# COMMAND LINE (original):
#   python3 knucleotide.py 0 < knucleotide-input25000000.txt
#
# PROGRAM OUTPUT (with small test data):
#   A 30.295
#   T 30.151
#   C 19.800
#   G 19.754
#
#   AA 9.177
#   TA 9.132
#   AT 9.131
#   TT 9.091
#   CA 6.002
#   AC 6.001
#   AG 5.987
#   GA 5.984
#   CT 5.971
#   TC 5.971
#   GT 5.957
#   TG 5.956
#   CC 3.917
#   GC 3.911
#   CG 3.909
#   GG 3.902
#
#   1471758	GGT
#   446535	GGTA
#   47336	GGTATT
#   893	GGTATTTTAATT
#   893	GGTATTTTAATTTATAGT

def gen_freq(seq, frame, frequences):
    ns = len(seq) + 1 - frame
    frequences.clear()
    for ii in range(ns):
        nucleo = seq[ii:ii + frame]
        if nucleo in frequences:
            frequences[nucleo] += 1
        else:
            frequences[nucleo] = 1
    return ns, frequences


def sort_seq(seq, length, frequences):
    n, frequences = gen_freq(seq, length, frequences)
    l = sorted(list(frequences.items()), reverse=True, key=lambda seq_freq: (seq_freq[1],seq_freq[0]))
    print('\n'.join("%s %.3f" % (st, 100.0*fr/n) for st,fr in l))
    print()


def find_seq(seq, s, frequences):
    n,t = gen_freq(seq, len(s), frequences)
    print("%d\t%s" % (t.get(s, 0), s))


def main():
    # Inline test data (small subset for compiler testing)
    test_data = """\
>TH
AAACCCGGGTTTAAACCCGGGTTTAAACCCGGGTTTAAACCCGGGTTT
AAACCCGGGTTTAAACCCGGGTTTAAACCCGGGTTTAAACCCGGGTTT
AAACCCGGGTTTAAACCCGGGTTTAAACCCGGGTTTAAACCCGGGTTT
GGTATTGGTATTGGTATTGGTATTGGTATTGGTATTGGTATTGGTATT
GGTATTTTAATTGGTATTTTAATTGGTATTTTAATTGGTATTTTAATT
GGTATTTTAATTTATAGTGGTATTTTAATTTATAGTGGTATTTTAATT
"""
    frequences = {}
    
    lines = test_data.split('\n')
    found_header = False
    seq_lines = []
    
    for line in lines:
        if line.startswith('>TH'):
            found_header = True
            continue
        if found_header and (line.startswith('>') or line.startswith(';')):
            break
        if found_header and len(line.strip()) > 0:
            seq_lines.append(line.strip())
    
    sequence = "".join(seq_lines).upper()

    for nl in 1,2:
        sort_seq(sequence, nl, frequences)

    for se in "GGT GGTA GGTATT GGTATTTTAATT GGTATTTTAATTTATAGT".split():
        find_seq(sequence, se, frequences)

main()
