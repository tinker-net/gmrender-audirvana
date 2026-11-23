# gmrender-audirvana — Audirvāna-focused experimental UPnP renderer

This is an **unofficial, experimental fork** of
[gmrender-resurrect](https://github.com/hzeller/gmrender-resurrect),
with additional UPnP/AVTransport interoperability fixes, PCM/DSD output
work, an optional ALSA mixer control, and a reversible DietPi installer.

**Development and tests:** Changes were developed with AI assistance and
reviewed through automated CI and hands-on tests on a Raspberry Pi running
DietPi/Debian Trixie with a Topping D10s USB DAC. The actual D10s reported
`DSD_U32_BE` at 88,200 Hz with two channels during DSD64 playback;
PCM playback, UPnP volume via ALSA, and repeated PCM-to-DSD changes have
also been observed working. These results do **not** establish general
compatibility, gapless performance on real albums, unattended long-run
stability or bit-perfect output under DAC attenuation.

**Start here:** [Raspberry Pi / DietPi quickstart](docs/PI-QUICKSTART.md).
The installer builds a separate service at `/opt/gmrender-audirvana`,
does not overwrite the original gmediarender installation, and provides
a command to switch back to the original renderer.

The tested development branch is **`preview-0.2`**; the repository's
`master` branch tracks the original gmrender-resurrect base. This project
is not affiliated with or endorsed by Audirvāna or the upstream maintainer.
The original open-source license and attributions are retained.

---

## Original gmrender-resurrect README (upstream text)

Headless UPnP Renderer
----------------------

I needed a small headless UPnP media renderer for Linux (for small footprint-use
in a Raspberry Pi or CuBox), but there was not much available.

Found this old project [GMediaRender][orig-project] - but it
was incomplete, several basic features missing and the project has been
abandoned several years ago.

So this is a fork of those sources to resurrect this renderer and add the
missing features to be useful (Original sources in [savannah cvs][orig-cvs]).
To distinguish this project from the original one, this is called
**[gmrender-resurrect](http://github.com/hzeller/gmrender-resurrect)**.

After many features added, this is now a very usable, headless UPnP
renderer that I would consider **stable**. You don't see many changes in this git - don't worry,
this project is not abandoned, but it just works as intended (I use it every day).
If you run into problems, please file bugs, write me directly or send a pull request; I am busy
but will try to respond.

If you have tested gmrender resurrect with your control point, **please add
it to the [Compatibility Wiki][compat-wiki] page** (even if there is a problem).
At this point, it should work with all media controllers, if not,
please file a bug so that we can figure out the issue and I can make it work
for you. See [INSTALL.md](./INSTALL.md) how to create a logfile that helps
to narrow down things.

If you're running this on a Raspberry Pi, you might be interested to connect
a little LCD display. Check out the **[upnp-display][]** github project.

Installation
------------
For installation instructions, see [INSTALL.md](./INSTALL.md)

You can reach me via <h.zeller@acm.org>.


[orig-project]: http://gmrender.nongnu.org/
[orig-cvs]:http://cvs.savannah.gnu.org/viewvc/gmrender/?root=gmrender
[compat-wiki]: https://github.com/hzeller/gmrender-resurrect/wiki/Compatibility
[upnp-display]: https://github.com/hzeller/upnp-display
[open-max-support]: https://github.com/hzeller/gmrender-resurrect/issues/33#issuecomment-23859699
