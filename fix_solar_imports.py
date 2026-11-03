import os

root = 'C:/Users/Ivan/Desktop/Lumen/Solar'
for dirpath, dirs, files in os.walk(root):
    for f in files:
        if not f.endswith('.py'):
            continue
        p = os.path.join(dirpath, f)
        s = open(p, encoding='utf-8').read()
        ns = s
        ns = ns.replace('from editor.', 'from Solar.')
        ns = ns.replace('import editor', 'import Solar')
        ns = ns.replace('"editor/', '"Solar/')
        ns = ns.replace("'editor/", "'Solar/")
        ns = ns.replace('editor_path', 'solar_path')
        if ns != s:
            open(p, 'w', encoding='utf-8').write(ns)
            print('fixed', p)
print('done')
