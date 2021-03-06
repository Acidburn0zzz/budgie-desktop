/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (c) 2008 Intel Corp.
 *
 * Author: Tomas Frydrych <tf@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma GCC diagnostic ignored "-Wdeprecated"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
/* Because GCC is tripping about the bitfields */
#pragma GCC diagnostic ignored "-Woverflow"

#include <config.h>

#include <meta/meta-plugin.h>
#include <meta/window.h>
#include <meta/meta-background-group.h>
#include <meta/meta-background-actor.h>
#include <meta/prefs.h>
#include <meta/keybindings.h>
#include <meta/util.h>
#include <glib/gi18n-lib.h>
#include <meta/meta-version.h>

#include <clutter/clutter.h>
#include <gmodule.h>
#include <string.h>

#include <gio/gdesktopappinfo.h>

#define MAXIMIZE_TIMEOUT   100
#define BACKGROUND_TIMEOUT 250
#define SWITCH_TIMEOUT    1

#include "plugin.h"
#include "impl.h"
#include "background.h"

#define ACTOR_DATA_KEY "MCCP-Default-actor-data"
#define SCREEN_TILE_PREVIEW_DATA_KEY "MCCP-Default-screen-tile-preview-data"

static GQuark actor_data_quark = 0;
static GQuark screen_tile_preview_data_quark = 0;


typedef struct _ScreenTilePreview
{
  ClutterActor   *actor;

  GdkRGBA        *preview_color;

  MetaRectangle   tile_rect;
} ScreenTilePreview;

/*
 * Actor private data accessor
 */
static void
free_actor_private (gpointer data)
{
  if (G_LIKELY (data != NULL))
    g_slice_free (ActorPrivate, data);
}

ActorPrivate *
get_actor_private (MetaWindowActor *actor)
{
  ActorPrivate *priv = g_object_get_qdata (G_OBJECT (actor), actor_data_quark);

  if (G_UNLIKELY (actor_data_quark == 0))
    actor_data_quark = g_quark_from_static_string (ACTOR_DATA_KEY);

  if (G_UNLIKELY (!priv))
    {
      priv = g_slice_new0 (ActorPrivate);

      g_object_set_qdata_full (G_OBJECT (actor),
                               actor_data_quark, priv,
                               free_actor_private);
    }

  return priv;
}

void
on_switch_workspace_effect_complete (ClutterTimeline *timeline, gpointer data)
{
  MetaPlugin               *plugin  = META_PLUGIN (data);
  BudgieWMPrivate *priv = BUDGIE_WM (plugin)->priv;
  MetaScreen *screen = meta_plugin_get_screen (plugin);
  GList *l = meta_get_window_actors (screen);

  while (l)
    {
      ClutterActor *a = l->data;
      MetaWindowActor *window_actor = META_WINDOW_ACTOR (a);
      ActorPrivate *apriv = get_actor_private (window_actor);

      if (apriv->orig_parent)
        {
          clutter_actor_reparent (a, apriv->orig_parent);
          apriv->orig_parent = NULL;
        }

      l = l->next;
    }

  clutter_actor_destroy (priv->desktop1);
  clutter_actor_destroy (priv->desktop2);

  priv->tml_switch_workspace1 = NULL;
  priv->tml_switch_workspace2 = NULL;
  priv->desktop1 = NULL;
  priv->desktop2 = NULL;

  meta_plugin_switch_workspace_completed (plugin);
}

void
on_monitors_changed (MetaScreen *screen,
                     MetaPlugin *plugin)
{
  BudgieWM *self = BUDGIE_WM (plugin);
  int i, n;
  clutter_actor_destroy_all_children (self->priv->background_group);


  n = meta_screen_get_n_monitors (screen);
  for (i = 0; i < n; i++)
    {
      ClutterActor *bg = budgie_background_new(screen, i);
      clutter_actor_add_child(self->priv->background_group, bg);
      clutter_actor_show(bg);
    }
}

void
switch_workspace (MetaPlugin *plugin,
                  gint from, gint to,
                  MetaMotionDirection direction)
{
  MetaScreen *screen;
  BudgieWMPrivate *priv = BUDGIE_WM (plugin)->priv;
  GList        *l;
  ClutterActor *workspace0  = clutter_group_new ();
  ClutterActor *workspace1  = clutter_group_new ();
  ClutterActor *stage;
  int           screen_width, screen_height;
  ClutterAnimation *animation;

  screen = meta_plugin_get_screen (plugin);
  stage = meta_get_stage_for_screen (screen);

  meta_screen_get_size (screen,
                        &screen_width,
                        &screen_height);

  clutter_actor_set_anchor_point (workspace1,
                                  screen_width,
                                  screen_height);
  clutter_actor_set_position (workspace1,
                              screen_width,
                              screen_height);

  clutter_actor_set_scale (workspace1, 0.0, 0.0);

  clutter_container_add_actor (CLUTTER_CONTAINER (stage), workspace1);
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), workspace0);

  if (from == to)
    {
      meta_plugin_switch_workspace_completed (plugin);
      return;
    }

  l = g_list_last (meta_get_window_actors (screen));

  while (l)
    {
      MetaWindowActor *window_actor = l->data;
      ActorPrivate    *apriv        = get_actor_private (window_actor);
      ClutterActor    *actor        = CLUTTER_ACTOR (window_actor);
      MetaWorkspace   *workspace;
      gint             win_workspace;

      workspace = meta_window_get_workspace (meta_window_actor_get_meta_window (window_actor));
      win_workspace = meta_workspace_index (workspace);

      if (win_workspace == to || win_workspace == from)
        {
          apriv->orig_parent = clutter_actor_get_parent (actor);

          clutter_actor_reparent (actor,
                  win_workspace == to ? workspace1 : workspace0);
          clutter_actor_show_all (actor);
          clutter_actor_raise_top (actor);
        }
      else if (win_workspace < 0)
        {
          /* Sticky window */
          apriv->orig_parent = NULL;
        }
      else
        {
          /* Window on some other desktop */
          clutter_actor_hide (actor);
          apriv->orig_parent = NULL;
        }

      l = l->prev;
    }

  priv->desktop1 = workspace0;
  priv->desktop2 = workspace1;

  animation = clutter_actor_animate (workspace0, CLUTTER_EASE_IN_SINE,
                                     SWITCH_TIMEOUT,
                                     "scale-x", 1.0,
                                     "scale-y", 1.0,
                                     NULL);
  priv->tml_switch_workspace1 = clutter_animation_get_timeline (animation);
  g_signal_connect (priv->tml_switch_workspace1,
                    "completed",
                    G_CALLBACK (on_switch_workspace_effect_complete),
                    plugin);

  animation = clutter_actor_animate (workspace1, CLUTTER_EASE_IN_SINE,
                                     SWITCH_TIMEOUT,
                                     "scale-x", 0.0,
                                     "scale-y", 0.0,
                                     NULL);
  priv->tml_switch_workspace2 = clutter_animation_get_timeline (animation);
}

/*
 * Tile preview private data accessor
 */
void
free_screen_tile_preview (gpointer data)
{
  ScreenTilePreview *preview = data;

  if (G_LIKELY (preview != NULL)) {
    clutter_actor_destroy (preview->actor);
    g_slice_free (ScreenTilePreview, preview);
  }
}

static ScreenTilePreview *
get_screen_tile_preview (MetaScreen *screen)
{
  ScreenTilePreview *preview = g_object_get_qdata (G_OBJECT (screen), screen_tile_preview_data_quark);

  if (G_UNLIKELY (screen_tile_preview_data_quark == 0))
    screen_tile_preview_data_quark = g_quark_from_static_string (SCREEN_TILE_PREVIEW_DATA_KEY);

  if (G_UNLIKELY (!preview))
    {
      preview = g_slice_new0 (ScreenTilePreview);

      preview->actor = clutter_actor_new ();
      clutter_actor_set_background_color (preview->actor, CLUTTER_COLOR_Blue);
      clutter_actor_set_opacity (preview->actor, 100);

      clutter_actor_add_child (meta_get_window_group_for_screen (screen), preview->actor);
      g_object_set_qdata_full (G_OBJECT (screen),
                               screen_tile_preview_data_quark, preview,
                               free_screen_tile_preview);
    }

  return preview;
}

void
show_tile_preview (MetaPlugin    *plugin,
                   MetaWindow    *window,
                   MetaRectangle *tile_rect,
                   int            tile_monitor_number)
{
  MetaScreen *screen = meta_plugin_get_screen (plugin);
  ScreenTilePreview *preview = get_screen_tile_preview (screen);
  ClutterActor *window_actor;

  if (CLUTTER_ACTOR_IS_VISIBLE (preview->actor)
      && preview->tile_rect.x == tile_rect->x
      && preview->tile_rect.y == tile_rect->y
      && preview->tile_rect.width == tile_rect->width
      && preview->tile_rect.height == tile_rect->height)
    return; /* nothing to do */

  clutter_actor_set_position (preview->actor, tile_rect->x, tile_rect->y);
  clutter_actor_set_size (preview->actor, tile_rect->width, tile_rect->height);

  clutter_actor_show (preview->actor);

  window_actor = CLUTTER_ACTOR (meta_window_get_compositor_private (window));
  clutter_actor_lower (preview->actor, window_actor);

  preview->tile_rect = *tile_rect;
}

void
hide_tile_preview (MetaPlugin *plugin)
{
  MetaScreen *screen = meta_plugin_get_screen (plugin);
  ScreenTilePreview *preview = get_screen_tile_preview (screen);

  clutter_actor_hide (preview->actor);
}

void
kill_switch_workspace (MetaPlugin     *plugin)
{
  BudgieWMPrivate *priv = BUDGIE_WM (plugin)->priv;

  if (priv->tml_switch_workspace1)
    {
      clutter_timeline_stop (priv->tml_switch_workspace1);
      clutter_timeline_stop (priv->tml_switch_workspace2);
      g_signal_emit_by_name (priv->tml_switch_workspace1, "completed", NULL);
    }
}

void
kill_window_effects (MetaPlugin      *plugin,
                     MetaWindowActor *window_actor)
{
  ActorPrivate *apriv;

  apriv = get_actor_private (window_actor);

  if (apriv->tml_minimize)
    {
      clutter_timeline_stop (apriv->tml_minimize);
      g_signal_emit_by_name (apriv->tml_minimize, "completed", NULL);
    }

  if (apriv->tml_map)
    {
      clutter_timeline_stop (apriv->tml_map);
      g_signal_emit_by_name (apriv->tml_map, "completed", NULL);
    }

  if (apriv->tml_destroy)
    {
      clutter_timeline_stop (apriv->tml_destroy);
      g_signal_emit_by_name (apriv->tml_destroy, "completed", NULL);
    }
}

static void
on_dialog_closed (GPid     pid,
                  gint     status,
                  gpointer user_data)
{
  MetaPlugin *plugin = user_data;
  gboolean ok;

  ok = g_spawn_check_exit_status (status, NULL);
  meta_plugin_complete_display_change (plugin, ok);
}

void
confirm_display_change (MetaPlugin *plugin)
{
  GPid pid;

  pid = meta_show_dialog ("--question",
                          "Does the display look OK?",
                          "20",
                          NULL,
                          "_Keep This Configuration",
                          "_Restore Previous Configuration",
                          "preferences-desktop-display",
                          0,
                          NULL, NULL);

  g_child_watch_add (pid, on_dialog_closed, plugin);
}
