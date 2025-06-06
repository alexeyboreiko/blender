/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor_physics
 * \brief Rigid Body world editing operators
 */

#include <cstdlib>
#include <cstring>

#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"

#ifdef WITH_BULLET
#  include "RBI_api.h"
#endif

#include "BKE_context.hh"
#include "BKE_report.hh"
#include "BKE_rigidbody.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "physics_intern.hh"

/* ********************************************** */
/* API */

/* check if there is an active rigid body world */
static bool rigidbody_world_active_poll(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  return (scene && scene->rigidbody_world);
}
static bool rigidbody_world_add_poll(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  return (scene && scene->rigidbody_world == nullptr);
}

/* ********************************************** */
/* OPERATORS - Management */

/* ********** Add RigidBody World **************** */

static wmOperatorStatus rigidbody_world_add_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  RigidBodyWorld *rbw;

  rbw = BKE_rigidbody_create_world(scene);
  //  BKE_rigidbody_validate_sim_world(scene, rbw, false);
  scene->rigidbody_world = rbw;

  /* Full rebuild of DEG! */
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update_ex(bmain, &scene->id, ID_RECALC_ANIMATION);

  return OPERATOR_FINISHED;
}

void RIGIDBODY_OT_world_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->idname = "RIGIDBODY_OT_world_add";
  ot->name = "Add Rigid Body World";
  ot->description = "Add Rigid Body simulation world to the current scene";

  /* callbacks */
  ot->exec = rigidbody_world_add_exec;
  ot->poll = rigidbody_world_add_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ********** Remove RigidBody World ************* */

static wmOperatorStatus rigidbody_world_remove_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  RigidBodyWorld *rbw = scene->rigidbody_world;

  /* sanity checks */
  if (ELEM(nullptr, scene, rbw)) {
    BKE_report(op->reports, RPT_ERROR, "No Rigid Body World to remove");
    return OPERATOR_CANCELLED;
  }

  BKE_rigidbody_free_world(scene);

  /* Full rebuild of DEG! */
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update_ex(bmain, &scene->id, ID_RECALC_ANIMATION);

  /* done */
  return OPERATOR_FINISHED;
}

void RIGIDBODY_OT_world_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->idname = "RIGIDBODY_OT_world_remove";
  ot->name = "Remove Rigid Body World";
  ot->description = "Remove Rigid Body simulation world from the current scene";

  /* callbacks */
  ot->exec = rigidbody_world_remove_exec;
  ot->poll = rigidbody_world_active_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ********************************************** */
/* UTILITY OPERATORS */

/* ********** Export RigidBody World ************* */

static wmOperatorStatus rigidbody_world_export_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  RigidBodyWorld *rbw = scene->rigidbody_world;
  char filepath[FILE_MAX];

  /* sanity checks */
  if (ELEM(nullptr, scene, rbw)) {
    BKE_report(op->reports, RPT_ERROR, "No Rigid Body World to export");
    return OPERATOR_CANCELLED;
  }
  rbDynamicsWorld *physics_world = BKE_rigidbody_world_physics(rbw);
  if (physics_world == nullptr) {
    BKE_report(
        op->reports, RPT_ERROR, "Rigid Body World has no associated physics data to export");
    return OPERATOR_CANCELLED;
  }

  RNA_string_get(op->ptr, "filepath", filepath);
#ifdef WITH_BULLET
  RB_dworld_export(physics_world, filepath);
#endif
  return OPERATOR_FINISHED;
}

static wmOperatorStatus rigidbody_world_export_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent * /*event*/)
{
  if (!RNA_struct_property_is_set(op->ptr, "relative_path")) {
    RNA_boolean_set(op->ptr, "relative_path", (U.flag & USER_RELPATHS) != 0);
  }

  if (RNA_struct_property_is_set(op->ptr, "filepath")) {
    return rigidbody_world_export_exec(C, op);
  }

  /* TODO: use the actual rigidbody world's name + .bullet instead of this temp crap */
  RNA_string_set(op->ptr, "filepath", "rigidbodyworld_export.bullet");
  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;
}

void RIGIDBODY_OT_world_export(wmOperatorType *ot)
{
  /* identifiers */
  ot->idname = "RIGIDBODY_OT_world_export";
  ot->name = "Export Rigid Body World";
  ot->description =
      "Export Rigid Body world to the simulator's own file-format "
      "(i.e. '.bullet' for Bullet Physics)";

  /* callbacks */
  ot->invoke = rigidbody_world_export_invoke;
  ot->exec = rigidbody_world_export_exec;
  ot->poll = rigidbody_world_active_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_SPECIAL,
                                 FILE_SAVE,
                                 WM_FILESEL_RELPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}
