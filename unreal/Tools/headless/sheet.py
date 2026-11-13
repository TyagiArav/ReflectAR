import sys
from PIL import Image
out, cols, scale = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
ims = [Image.open(f) for f in sys.argv[4:]]
w, h = ims[0].size
w2, h2 = int(w * scale), int(h * scale)
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w2, rows * h2))
for i, im in enumerate(ims):
    sheet.paste(im.resize((w2, h2), Image.LANCZOS), ((i % cols) * w2, (i // cols) * h2))
sheet.save(out)
