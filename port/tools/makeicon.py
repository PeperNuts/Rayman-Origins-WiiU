# Original line-art icon of a generic tablet-style game controller (white lines on a dark body),
# matching the style of the game's controller icons: 256x256 colour + alpha silhouette.
from PIL import Image, ImageDraw, ImageFilter
S = 4; W = 256 * S
def rr(d, box, r, **kw): d.rounded_rectangle([v * S for v in box], radius=r * S, **kw)
def circ(d, cx, cy, r, **kw): d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], **kw)
LW = 7
mask = Image.new('L', (W, W), 0); md = ImageDraw.Draw(mask)
col = Image.new('RGB', (W, W), (0x30, 0x2a, 0x10)); cd = ImageDraw.Draw(col)
body = (14, 62, 242, 200)
# grips below the body
rr(md, body, 34, fill=255)
# outline of body + grips
cd_outline = (255, 255, 255)
rr(cd, body, 34, fill=(0x30, 0x2a, 0x10), outline=cd_outline, width=LW * S)
# screen
rr(cd, (70, 80, 186, 182), 10, outline=cd_outline, width=LW * S)
# left stick, d-pad
circ(cd, 42, 100, 17, outline=cd_outline, width=LW * S); circ(cd, 42, 100, 6, fill=cd_outline)
for (x0, y0, x1, y1) in ((36, 128, 48, 168), (22, 142, 62, 154)):
    rr(cd, (x0, y0, x1, y1), 2, outline=cd_outline, width=5 * S)
# right stick, face buttons
circ(cd, 214, 100, 17, outline=cd_outline, width=LW * S); circ(cd, 214, 100, 6, fill=cd_outline)
for (bx, by) in ((214, 132), (214, 164), (198, 148), (230, 148)):
    circ(cd, bx, by, 8, outline=cd_outline, width=5 * S)
# shoulder buttons
mask = mask.filter(ImageFilter.MaxFilter(3))
col = col.resize((256, 256), Image.LANCZOS); mask = mask.resize((256, 256), Image.LANCZOS)
img = col.convert('RGBA'); img.putalpha(mask)
import sys; img.save(sys.argv[1] if len(sys.argv) > 1 else 'gamepad_icon.png')
