#!/usr/bin/env python3
"""Pack velvet-ui.html + shots/*.png + web/velvet-web.js into ONE shareable file.
usage: tools/bundle-html.py [out.html]   (default: velvet-share.html)"""
import base64, os, re, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'velvet-share.html')
html = open(os.path.join(root, 'velvet-ui.html'), encoding='utf-8').read()

def inline_img(m):
    path = os.path.join(root, m.group(1))
    data = base64.b64encode(open(path, 'rb').read()).decode()
    mime = 'image/jpeg' if m.group(1).endswith('.jpg') else 'image/png'
    return f'src="data:{mime};base64,{data}"'

html = re.sub(r'src="((?:shots|web)/[^"]+\.(?:png|jpg))"', inline_img, html)
# fonts referenced from CSS: url(web/x.otf)
def inline_font(m):
    data = base64.b64encode(open(os.path.join(root, m.group(1)), 'rb').read()).decode()
    return f'url(data:font/otf;base64,{data})'
html = re.sub(r'url\((web/[^)]+\.otf)\)', inline_font, html)

js = open(os.path.join(root, 'web/velvet-web.js'), encoding='utf-8').read().replace('</script', '<\\/script')
loader = re.compile(r"  const sc=document\.createElement\('script'\); sc\.src='web/velvet-web\.js';\n"
                    r"  sc\.onerror=.*?\n  document\.body\.appendChild\(sc\);\n", re.S)
assert loader.search(html), 'loader block not found'
html = loader.sub('', html)
# the app runs right after the page script has set up window.Module
html = html.replace('</body>', '<script>\n' + js + '\n</script>\n</body>', 1)
open(out, 'w', encoding='utf-8').write(html)
print(f'{out}: {os.path.getsize(out) / 1e6:.1f} MB')
