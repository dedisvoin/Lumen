import io, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
p = r'C:\Users\Ivan\Desktop\Lumen\compiler\core\ErrorBase.cpp'
L = open(p, encoding='utf-8').read().splitlines()
print('TOTAL_LINES', len(L))
for i in range(1380, len(L)):
    s = L[i]
    tag = ''
    if s.rstrip().endswith('{'): tag = '  <<OPEN'
    if s.rstrip() == '}': tag = '  <<CLOSE'
    if any(k in s for k in ('REF_','INVALID_CAST','namespace')):
        tag += '  <<MATCH'
    print('%5d%s| %s' % (i+1, tag, s))
