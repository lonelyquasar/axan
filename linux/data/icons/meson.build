# Copyright © 2019, 2020, 2021 Christian Persch
#
# This programme is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or (at your
# option) any later version.
#
# This programme is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this programme.  If not, see <https://www.gnu.org/licenses/>.

public_icons_themes = [
  'hicolor',
]

# Source filenames encode their install path as `<theme>_<context>_<size>_<name>`
# and the foreach below reorders the underscore-split parts into
# `<theme>/<size>/<context>/<name>` (see the rename loop). Keep that 4-part
# shape for anything added here, or the rename math silently misfiles it.
#
# The main app icon is axan's own artwork (opaque variant), installed under the
# `sh.axan.Axan` name the desktop file and AXAN_ICON_NAME in the C source look
# up at runtime. We ship the scalable SVG plus a pre-rendered PNG ladder so the
# icon stays crisp at the small window-list sizes that don't always rasterise
# the SVG. The Preferences and symbolic icons are still the upstream
# gnome-terminal art pending a dedicated variant, but they install under the
# sh.axan.Axan.* names the desktop files and GNOME Shell actually look up.
public_icons_sources = [
  'hicolor_apps_scalable_sh.axan.Axan.svg',
  'hicolor_apps_16x16_sh.axan.Axan.png',
  'hicolor_apps_24x24_sh.axan.Axan.png',
  'hicolor_apps_32x32_sh.axan.Axan.png',
  'hicolor_apps_48x48_sh.axan.Axan.png',
  'hicolor_apps_64x64_sh.axan.Axan.png',
  'hicolor_apps_128x128_sh.axan.Axan.png',
  'hicolor_apps_256x256_sh.axan.Axan.png',
  'hicolor_apps_512x512_sh.axan.Axan.png',
  'hicolor_apps_scalable_sh.axan.Axan.Preferences.svg',
  'hicolor_apps_symbolic_sh.axan.Axan-symbolic.svg',
  'hicolor_apps_symbolic_sh.axan.Axan.Preferences-symbolic.svg',
]

public_icons_renames = []

foreach icon : public_icons_sources
  components = icon.split('_')
  public_icons_renames += components[0] / components[2] / components[1] / components[3]
endforeach

icondir = gt_datadir / 'icons'
pkgicondir = gt_pkgdatadir / 'icons'

install_data(
  sources: public_icons_sources,
  rename: public_icons_renames,
  install_dir: icondir,
  install_tag: 'runtime',
)

foreach theme : public_icons_themes
  meson.add_install_script(
    'meson_updateiconcache.py',
    icondir / theme,
  )
endforeach
