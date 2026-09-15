import io, pdb
import _colorize
_colorize.can_colorize = lambda *a, **k: True
out = io.StringIO()
p = pdb.Pdb(stdout=out, colorize=True)
p.set_trace(commands=["ll", "c"])
print("esc", "\x1b" in out.getvalue())
