#!/usr/bin/python3
# Reproduce the generated-avatar path gnome-initial-setup and
# gnome-control-center use, in isolation:
#
#   AdwAvatar -> gtk_snapshot -> gsk_renderer_render_texture()
#             -> gdk_pixbuf_get_from_texture() -> PNG
#
# Same calls, same 512px size, same tEXt marker as
# gis-account-page-local.c:628 and cc-user-page.c:245. A correct result is a
# ~14 KB circle with an initial in it; a failed dmabuf download leaves the
# pixel buffer untouched, so the PNG comes out either blank (~2 KB) or full of
# whatever was in that memory (~130 KB of noise).
#
# Needs a Wayland display. The board has no seat over SSH, so run it inside the
# live GDM greeter session -- see measure.sh.
import sys, gi
def log(*a): print(*a, file=sys.stderr, flush=True)
log("step: imports")
gi.require_version('Gtk','4.0'); gi.require_version('Adw','1')
gi.require_version('Gdk','4.0'); gi.require_version('Gsk','4.0')
from gi.repository import Gtk, Adw, Gdk, Gsk, Graphene, GLib
SIZE=512
out=sys.argv[1]
log("step: Adw.init"); Adw.init()
log("step: window")
win=Gtk.Window()
avatar=Adw.Avatar(size=128, text="Pencil", show_initials=True)
win.set_child(avatar)
log("step: present"); win.present(); log("step: presented")
loop=GLib.MainLoop()
def run():
    try:
        log("step: measure")
        rs=avatar.get_size()
        avatar.measure(Gtk.Orientation.HORIZONTAL, rs)
        avatar.allocate(rs, rs, -1, None)
        ok,tf=avatar.compute_transform(avatar.get_first_child()); assert ok
        s=Gtk.Snapshot(); s.transform_matrix(tf)
        Gtk.Widget.do_snapshot(avatar, s)
        p=s.to_paintable(Graphene.Size().init(rs,rs))
        s=Gtk.Snapshot(); p.snapshot(s, SIZE, SIZE)
        node=s.to_node()
        r=win.get_native().get_renderer()
        log("RENDERER:", type(r).__name__)
        log("step: render_texture")
        tex=r.render_texture(node, Graphene.Rect().init(-1,0,SIZE,SIZE))
        log("step: download")
        pb=Gdk.pixbuf_get_from_texture(tex)
        pb.savev(out,"png",["tEXt::source"],["gnome-generated"])
        log("WROTE", out)
    except Exception as e:
        log("ERROR:", e)
    loop.quit(); return False
GLib.timeout_add(2000, run)
GLib.timeout_add(120000, lambda:(loop.quit(),False)[1])
log("step: mainloop"); loop.run(); log("step: done")
