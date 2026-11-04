# Original line-art icon of a generic two-handled game controller (dual sticks), in the style
# of the game's controller icons: white lines on a dark body, alpha silhouette.
from PIL import Image, ImageDraw, ImageFilter
S = 4; W = 256 * S
BG = (0x30, 0x2a, 0x10); WH = (255, 255, 255); LW = 7
def circ(d, cx, cy, r, **kw): d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], **kw)
def rr(d, box, r, **kw): d.rounded_rectangle([v * S for v in box], radius=r * S, **kw)
mask = Image.new('L', (W, W), 0); md = ImageDraw.Draw(mask)
col = Image.new('RGB', (W, W), BG); cd = ImageDraw.Draw(col)
def body(d, **kw):
    # handles (circles low on each side) joined by a wide rounded body
    circ(d, 58, 168, 42, **kw); circ(d, 198, 168, 42, **kw)
    rr(d, (18, 72, 238, 168), 48, **kw)
body(md, fill=255)
mask = mask.filter(ImageFilter.MaxFilter(1 + 2 * LW * S // 2 | 1))
# outline: draw the white silhouette, then the body shrunk by the line width in dark
ImageDraw.Draw(col).bitmap((0, 0), mask, fill=WH)
inner = Image.new('L', (W, W), 0); idr = ImageDraw.Draw(inner); body(idr, fill=255)
inner = inner.filter(ImageFilter.MinFilter(1 + 2 * (LW * S // 2) | 1))
col.paste(BG, (0, 0), inner)
cd = ImageDraw.Draw(col)
circ(cd, 66, 104, 18, outline=WH, width=LW * S); circ(cd, 66, 104, 6, fill=WH)   # left stick
for (x0, y0, x1, y1) in ((92, 124, 104, 164), (78, 138, 118, 150)):                 # d-pad
    rr(cd, (x0, y0, x1, y1), 2, outline=WH, width=5 * S)
for (bx, by) in ((190, 88), (190, 120), (174, 104), (206, 104)):                      # face buttons
    circ(cd, bx, by, 8, outline=WH, width=5 * S)
circ(cd, 160, 146, 18, outline=WH, width=LW * S); circ(cd, 160, 146, 6, fill=WH)  # right stick
for x in (116, 140): circ(cd, x, 96, 4, fill=WH)                                   # - +
col = col.resize((256, 256), Image.LANCZOS); mask = mask.resize((256, 256), Image.LANCZOS)
img = col.convert('RGBA'); img.putalpha(mask)
img.save('/tmp/claude-0/img/pro_icon.png')
