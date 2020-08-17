/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup spinfo
 */

#include <limits.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_dynstr.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_report.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_screen.h"
#include "ED_select_utils.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "CLG_log.h"
#include "info_intern.h"

static bool ED_operator_info_clog_active(bContext *C)
{
  const SpaceInfo *sinfo = CTX_wm_space_info(C);
  return ED_operator_info_active(C) && sinfo->view == INFO_VIEW_CLOG;
}

bool is_clog_record_visible(const CLG_LogRecord *record, const SpaceInfo *sinfo)
{
  /* general search */
  const SpaceInfoFilter *search_filter = sinfo->search_filter;
  if (!info_match_string_filter(search_filter->search_string,
                                record->message,
                                search_filter->flag & INFO_FILTER_USE_MATCH_CASE,
                                search_filter->flag & INFO_FILTER_USE_GLOB,
                                search_filter->flag & INFO_FILTER_USE_MATCH_REVERSE)) {
    return false;
  }

  /* filter log severity (flag like)  */
  if (!(sinfo->log_severity_mask & INFO_CLOG_SEVERITY_DEBUG) &&
      record->severity == CLG_SEVERITY_DEBUG) {
    return false;
  }
  if (!(sinfo->log_severity_mask & INFO_CLOG_SEVERITY_VERBOSE) &&
      record->severity == CLG_SEVERITY_VERBOSE) {
    return false;
  }
  if (!(sinfo->log_severity_mask & INFO_CLOG_SEVERITY_INFO) &&
      record->severity == CLG_SEVERITY_INFO) {
    return false;
  }
  if (!(sinfo->log_severity_mask & INFO_CLOG_SEVERITY_WARN) &&
      record->severity == CLG_SEVERITY_WARN) {
    return false;
  }
  if (!(sinfo->log_severity_mask & INFO_CLOG_SEVERITY_ERROR) &&
      record->severity == CLG_SEVERITY_ERROR) {
    return false;
  }
  if (!(sinfo->log_severity_mask & INFO_CLOG_SEVERITY_FATAL) &&
      record->severity == CLG_SEVERITY_FATAL) {
    return false;
  }

  /* filter verbosity */
  if (sinfo->use_log_filter & INFO_FILTER_CLOG_LEVEL) {
    if (sinfo->filter_log_level < record->verbosity) {
      return false;
    }
  }

  /* filter log type */
  if (sinfo->use_log_filter & INFO_FILTER_CLOG_TYPE) {
    LISTBASE_FOREACH (struct SpaceInfoFilter *, filter, &sinfo->filter_log_type) {
      if (!info_match_string_filter(filter->search_string,
                                    record->type->identifier,
                                    filter->flag & INFO_FILTER_USE_MATCH_CASE,
                                    filter->flag & INFO_FILTER_USE_GLOB,
                                    filter->flag & INFO_FILTER_USE_MATCH_REVERSE)) {
        return false;
      }
    }
  }

  /* filter log function */
  if (sinfo->use_log_filter & INFO_FILTER_CLOG_FUNCTION) {
    LISTBASE_FOREACH (struct SpaceInfoFilter *, filter, &sinfo->filter_log_function) {
      if (!info_match_string_filter(filter->search_string,
                                    record->function,
                                    filter->flag & INFO_FILTER_USE_MATCH_CASE,
                                    filter->flag & INFO_FILTER_USE_GLOB,
                                    filter->flag & INFO_FILTER_USE_MATCH_REVERSE)) {
        return false;
      }
    }
  }

  /* filter file line */
  if (sinfo->use_log_filter & INFO_FILTER_CLOG_FILE_LINE) {
    LISTBASE_FOREACH (struct SpaceInfoFilter *, filter, &sinfo->filter_log_file_line) {
      if (!info_match_string_filter(filter->search_string,
                                    record->file_line,
                                    filter->flag & INFO_FILTER_USE_MATCH_CASE,
                                    filter->flag & INFO_FILTER_USE_GLOB,
                                    filter->flag & INFO_FILTER_USE_MATCH_REVERSE)) {
        return false;
      }
    }
  }

  return true;
}

static void log_records_select_all(CLG_LogRecordList *records, const SpaceInfo *sinfo, int action)
{
  if (action == SEL_TOGGLE) {
    action = SEL_SELECT;
    for (CLG_LogRecord *record = records->last; record; record = record->prev) {
      if (is_clog_record_visible(record, sinfo) && (record->flag & CLG_SELECT)) {
        action = SEL_DESELECT;
        break;
      }
    }
  }

  for (CLG_LogRecord *record = records->last; record; record = record->prev) {
    if (is_clog_record_visible(record, sinfo)) {
      switch (action) {
        case SEL_SELECT:
          record->flag |= CLG_SELECT;
          break;
        case SEL_DESELECT:
          record->flag &= ~CLG_SELECT;
          break;
        case SEL_INVERT:
          record->flag ^= CLG_SELECT;
          break;
        default:
          BLI_assert(0);
      }
    }
  }
}

static int select_clog_pick_exec(bContext *C, wmOperator *op)
{
  const int clog_index = RNA_int_get(op->ptr, "clog_index");
  const bool extend = RNA_boolean_get(op->ptr, "extend");
  const bool use_range = RNA_boolean_get(op->ptr, "extend_range");
  const bool deselect_all = RNA_boolean_get(op->ptr, "deselect_all");

  SpaceInfo *sinfo = CTX_wm_space_info(C);

  CLG_LogRecordList *records = CLG_log_records_get();
  CLG_LogRecord *record = BLI_findlink((const struct ListBase *)records, clog_index);

  if (clog_index == INDEX_INVALID) {  // click in empty area
    log_records_select_all(records, sinfo, SEL_DESELECT);
    info_area_tag_redraw(C);
    return OPERATOR_FINISHED;
  }

  if (!record) {
    return OPERATOR_CANCELLED;
  }

  const CLG_LogRecord *active_item = BLI_findlink((const struct ListBase *)records,
                                                  sinfo->active_index);
  const bool is_active_item_selected = active_item ? active_item->flag & CLG_SELECT : false;

  if (deselect_all) {
    log_records_select_all(records, sinfo, SEL_DESELECT);
  }

  if (active_item == NULL) {
    record->flag |= CLG_SELECT;
    sinfo->active_index = clog_index;
    info_area_tag_redraw(C);
    return OPERATOR_FINISHED;
  }

  if (use_range) {
    if (is_active_item_selected) {
      if (clog_index < sinfo->active_index) {
        for (CLG_LogRecord *i = record; i && i->prev != active_item; i = i->next) {
          i->flag |= CLG_SELECT;
        }
      }
      else {
        for (CLG_LogRecord *record_iter = record; record_iter && record_iter->next != active_item;
             record_iter = record_iter->prev) {
          record_iter->flag |= CLG_SELECT;
        }
      }
      info_area_tag_redraw(C);
      return OPERATOR_FINISHED;
    }
    log_records_select_all(records, sinfo, SEL_DESELECT);
    record->flag |= CLG_SELECT;
    sinfo->active_index = clog_index;
    info_area_tag_redraw(C);
    return OPERATOR_FINISHED;
  }

  if (extend && (record->flag & CLG_SELECT) && clog_index == sinfo->active_index) {
    record->flag &= ~CLG_SELECT;
  }
  else {
    record->flag |= CLG_SELECT;
    sinfo->active_index = BLI_findindex((const struct ListBase *)records, record);
  }
  info_area_tag_redraw(C);
  return OPERATOR_FINISHED;
}

static int select_clog_pick_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  SpaceInfo *sinfo = CTX_wm_space_info(C);
  ARegion *region = CTX_wm_region(C);
  CLG_LogRecordList *records = CLG_log_records_get();
  CLG_LogRecord *record;

  BLI_assert(sinfo->view == INFO_VIEW_CLOG);
  record = info_text_pick(sinfo, region, NULL, event->mval[1]);

  if (record == NULL) {
    RNA_int_set(op->ptr, "clog_index", INDEX_INVALID);
  }
  else {
    RNA_int_set(op->ptr, "clog_index", BLI_findindex((const struct ListBase *)records, record));
  }

  return select_clog_pick_exec(C, op);
}

void INFO_OT_clog_select_pick(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select CLG_LogRecord";
  ot->description = "Select records by index";
  ot->idname = "INFO_OT_clog_select_pick";

  /* api callbacks */
  ot->poll = ED_operator_info_clog_active;
  ot->invoke = select_clog_pick_invoke;
  ot->exec = select_clog_pick_exec;

  /* flags */
  /* ot->flag = OPTYPE_REGISTER; */

  /* properties */
  PropertyRNA *prop;
  RNA_def_int(ot->srna,
              "clog_index",
              0,
              INDEX_INVALID,
              INT_MAX,
              "Log Record",
              "Index of the log record",
              0,
              INT_MAX);
  prop = RNA_def_boolean(ot->srna, "extend", false, "Extend", "Extend record selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(
      ot->srna, "extend_range", false, "Extend range", "Select a range from active element");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna,
                         "deselect_all",
                         true,
                         "Deselect On Nothing",
                         "Deselect all when nothing under the cursor");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static int clog_select_all_exec(bContext *C, wmOperator *op)
{
  SpaceInfo *sinfo = CTX_wm_space_info(C);
  CLG_LogRecordList *records = CLG_log_records_get();

  int action = RNA_enum_get(op->ptr, "action");
  log_records_select_all(records, sinfo, action);
  info_area_tag_redraw(C);

  return OPERATOR_FINISHED;
}

void INFO_OT_clog_select_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "(De)select All";
  ot->description = "Change selection of all visible records";
  ot->idname = "INFO_OT_clog_select_all";

  /* api callbacks */
  ot->poll = ED_operator_info_clog_active;
  ot->exec = clog_select_all_exec;

  /* properties */
  WM_operator_properties_select_action(ot, SEL_SELECT, true);
}

/* box_select operator */
static int box_select_exec(bContext *C, wmOperator *op)
{
  SpaceInfo *sinfo = CTX_wm_space_info(C);
  ARegion *region = CTX_wm_region(C);
  CLG_LogRecordList *records = CLG_log_records_get();
  CLG_LogRecord *record_min, *record_max;
  rcti rect;

  WM_operator_properties_border_to_rcti(op, &rect);

  const eSelectOp sel_op = RNA_enum_get(op->ptr, "mode");
  const int select = (sel_op != SEL_OP_SUB);
  if (SEL_OP_USE_PRE_DESELECT(sel_op)) {
    LISTBASE_FOREACH (CLG_LogRecord *, record, records) {
      if (!is_clog_record_visible(record, sinfo)) {
        continue;
      }
      record->flag &= ~CLG_SELECT;
    }
  }

  BLI_assert(sinfo->view == INFO_VIEW_CLOG);
  record_min = info_text_pick(sinfo, region, NULL, rect.ymax);
  record_max = info_text_pick(sinfo, region, NULL, rect.ymin);

  if (record_min == NULL && record_max == NULL) {
    log_records_select_all(records, sinfo, SEL_DESELECT);
  }
  else {
    /* get the first record if none found */
    if (record_min == NULL) {
      // printf("find_min\n");
      LISTBASE_FOREACH (CLG_LogRecord *, record, records) {
        if (is_clog_record_visible(record, sinfo)) {
          record_min = record;
          break;
        }
      }
    }

    if (record_max == NULL) {
      // printf("find_max\n");
      for (CLG_LogRecord *record = records->last; record; record = record->prev) {
        if (is_clog_record_visible(record, sinfo)) {
          record_max = record;
          break;
        }
      }
    }

    if (record_min == NULL || record_max == NULL) {
      return OPERATOR_CANCELLED;
    }

    for (CLG_LogRecord *record = record_min; (record != record_max->next); record = record->next) {
      if (!is_clog_record_visible(record, sinfo)) {
        continue;
      }
      SET_FLAG_FROM_TEST(record->flag, select, CLG_SELECT);
    }
  }

  info_area_tag_redraw(C);
  return OPERATOR_FINISHED;
}

/* ****** Box Select ****** */
void INFO_OT_clog_select_box(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Box Select";
  ot->description = "Toggle box selection";
  ot->idname = "INFO_OT_clog_select_box";

  /* api callbacks */
  ot->invoke = WM_gesture_box_invoke;
  ot->exec = box_select_exec;
  ot->modal = WM_gesture_box_modal;
  ot->cancel = WM_gesture_box_cancel;

  ot->poll = ED_operator_info_clog_active;

  /* flags */
  /* ot->flag = OPTYPE_REGISTER; */

  /* properties */
  WM_operator_properties_gesture_box(ot);
  WM_operator_properties_select_operation_simple(ot);
}

static int clog_delete_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceInfo *sinfo = CTX_wm_space_info(C);
  CLG_LogRecordList *records = CLG_log_records_get();

  CLG_LogRecord *record, *record_next;

  for (record = records->first; record;) {
    record_next = record->next;

    if (is_clog_record_visible(record, sinfo) && (record->flag & CLG_SELECT)) {
      printf("NOT IMPLEMENTED YET");
      //      BLI_remlink((struct ListBase *)records, record);
      //      MEM_freeN((void *)record->message);
      //      MEM_freeN(record);
    }

    record = record_next;
  }
  info_area_tag_redraw(C);

  return OPERATOR_FINISHED;
}

void INFO_OT_clog_delete(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Log Records";
  ot->description = "Delete selected log records";
  ot->idname = "INFO_OT_clog_delete";

  /* api callbacks */
  ot->poll = ED_operator_info_clog_active;
  ot->exec = clog_delete_exec;

  /* flags */
  /*ot->flag = OPTYPE_REGISTER;*/

  /* properties */
}

typedef enum eClogCopy {
  CLOG_COPY_VISBLE = 0,
  CLOG_COPY_MESSAGE,
  CLOG_COPY_FILE_LINE,
  CLOG_COPY_FILE_LINE_SHORT,
} eClogCopy;

static int clog_copy_exec(bContext *C, wmOperator *op)
{
  SpaceInfo *sinfo = CTX_wm_space_info(C);
  CLG_LogRecordList *records = CLG_log_records_get();
  eClogCopy copy_type = RNA_enum_get(op->ptr, "method");
  CLG_LogRecord *record;

  DynStr *buf_dyn = BLI_dynstr_new();
  char *buf_str;

  for (record = records->first; record; record = record->next) {
    if (is_clog_record_visible(record, sinfo) && (record->flag & CLG_SELECT)) {
      switch (copy_type) {
        case CLOG_COPY_VISBLE: {
          char *log = clog_record_sprintfN(record, sinfo);
          BLI_dynstr_append(buf_dyn, log);
          BLI_dynstr_append(buf_dyn, "\n");
          break;
        }
        case CLOG_COPY_MESSAGE: {
          BLI_dynstr_append(buf_dyn, record->message);
          BLI_dynstr_append(buf_dyn, "\n");
          break;
        }
        case CLOG_COPY_FILE_LINE: {
          BLI_dynstr_append(buf_dyn, record->file_line);
          BLI_dynstr_append(buf_dyn, "\n");
          break;
        }
        case CLOG_COPY_FILE_LINE_SHORT: {
          BLI_dynstr_append(buf_dyn, BLI_path_basename(record->file_line));
          BLI_dynstr_append(buf_dyn, "\n");
          break;
        }
        default:
          BLI_assert(false);
      }
    }
  }

  buf_str = BLI_dynstr_get_cstring(buf_dyn);
  BLI_dynstr_free(buf_dyn);

  WM_clipboard_text_set(buf_str, 0);

  MEM_freeN(buf_str);
  return OPERATOR_FINISHED;
}

void INFO_OT_clog_copy(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Copy Log Message to Clipboard";
  ot->description = "Copy selected log records to Clipboard";
  ot->idname = "INFO_OT_clog_copy";

  /* api callbacks */
  ot->poll = ED_operator_info_clog_active;
  ot->exec = clog_copy_exec;

  /* flags */
  /*ot->flag = OPTYPE_REGISTER;*/

  /* properties */
  static const EnumPropertyItem clog_copy_items[] = {
      {CLOG_COPY_VISBLE, "COPY_VISIBLE", 0, "", ""},
      {CLOG_COPY_MESSAGE, "COPY_MESSAGE", 0, "", ""},
      {CLOG_COPY_FILE_LINE, "COPY_PATH", 0, "", ""},
      {CLOG_COPY_FILE_LINE_SHORT, "COPY_BASENAME", 0, "", ""},
      {0, NULL, 0, NULL, NULL},
  };

  PropertyRNA *prop;
  prop = RNA_def_enum(ot->srna, "method", clog_copy_items, CLOG_COPY_VISBLE, "Method", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

typedef enum eClogFilterMode {
  CLOG_FILTER_FUNCTION = 0,
  CLOG_FILTER_FILE,
  CLOG_FILTER_LINE,
  CLOG_FILTER_LOG_TYPE,
} eClogFilterMode;

static struct SpaceInfoFilter *is_filter_duplicate(const ListBase *list,
                                                   const SpaceInfoFilter *filter)
{
  LISTBASE_FOREACH (SpaceInfoFilter *, filter_iter, list) {
    if (info_match_string_filter(filter_iter->search_string,
                                 filter->search_string,
                                 filter_iter->flag & INFO_FILTER_USE_MATCH_CASE,
                                 filter_iter->flag & INFO_FILTER_USE_GLOB,
                                 false)) {
      return filter_iter;
    }
  }
  return NULL;
}

static int clog_filter_exec(bContext *C, wmOperator *op)
{
  SpaceInfo *sinfo = CTX_wm_space_info(C);
  CLG_LogRecordList *records = CLG_log_records_get();
  eClogFilterMode filter_type = RNA_enum_get(op->ptr, "method");

  CLG_LogRecord *record;

  for (record = records->first; record; record = record->next) {
    if (is_clog_record_visible(record, sinfo) && (record->flag & CLG_SELECT)) {
      SpaceInfoFilter *filter = MEM_callocN(sizeof(*filter), __func__);
      filter->flag = INFO_FILTER_USE_MATCH_CASE | INFO_FILTER_USE_MATCH_REVERSE;
      switch (filter_type) {
        case CLOG_FILTER_FILE: {
          const char *basename = BLI_path_basename(record->file_line);
          int file_name_len = strlen(basename);
          /* remove line number */
          BLI_assert(strstr(basename, ":") != NULL);
          while (basename[file_name_len - 1] != ':') {
            file_name_len -= 1;
          }
          BLI_strncpy(filter->search_string, basename, file_name_len);
          const SpaceInfoFilter *filter_dup = is_filter_duplicate(&sinfo->filter_log_file_line,
                                                                  filter);
          if (filter_dup == NULL) {
            BLI_addtail(&sinfo->filter_log_file_line, filter);
          }
          else {
            BKE_reportf(op->reports,
                        RPT_INFO,
                        "File filter: %s is duplicate of filter: %s",
                        filter->search_string,
                        filter_dup->search_string);
            MEM_freeN(filter);
          }
          sinfo->use_log_filter |= INFO_FILTER_CLOG_FILE_LINE;
          break;
        }
        case CLOG_FILTER_LINE: {
          STRNCPY(filter->search_string, BLI_path_basename(record->file_line));
          const SpaceInfoFilter *filter_dup = is_filter_duplicate(&sinfo->filter_log_file_line,
                                                                  filter);
          if (filter_dup == NULL) {
            BLI_addtail(&sinfo->filter_log_file_line, filter);
          }
          else {
            BKE_reportf(op->reports,
                        RPT_INFO,
                        "Line filter: %s is duplicate of filter: %s",
                        filter->search_string,
                        filter_dup->search_string);
            MEM_freeN(filter);
          }
          sinfo->use_log_filter |= INFO_FILTER_CLOG_FILE_LINE;
          break;
        }
        case CLOG_FILTER_FUNCTION: {
          STRNCPY(filter->search_string, record->function);
          const SpaceInfoFilter *filter_dup = is_filter_duplicate(&sinfo->filter_log_function,
                                                                  filter);
          if (filter_dup == NULL) {
            BLI_addtail(&sinfo->filter_log_function, filter);
          }
          else {
            BKE_reportf(op->reports,
                        RPT_INFO,
                        "Function filter: %s is duplicate of filter: %s",
                        filter->search_string,
                        filter_dup->search_string);
            MEM_freeN(filter);
          }
          sinfo->use_log_filter |= INFO_FILTER_CLOG_FUNCTION;
          break;
        }
        case CLOG_FILTER_LOG_TYPE: {
          STRNCPY(filter->search_string, record->type->identifier);
          const SpaceInfoFilter *filter_dup = is_filter_duplicate(&sinfo->filter_log_type, filter);
          if (filter_dup == NULL) {
            BLI_addtail(&sinfo->filter_log_type, filter);
          }
          else {
            BKE_reportf(op->reports,
                        RPT_INFO,
                        "Function filter: %s is duplicate of filter: %s",
                        filter->search_string,
                        filter_dup->search_string);
            MEM_freeN(filter);
          }
          sinfo->use_log_filter |= INFO_FILTER_CLOG_TYPE;
          break;
        }
        default:
          BLI_assert(0);
      }
    }
  }

  info_area_tag_redraw(C);

  return OPERATOR_FINISHED;
}

void INFO_OT_clog_filter(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Log Filter";
  ot->description =
      "Add and enable log filter based on selected logs. Will not add filter if log is already "
      "muted by another filter";
  ot->idname = "INFO_OT_clog_filter";

  /* api callbacks */
  ot->poll = ED_operator_info_clog_active;
  ot->exec = clog_filter_exec;

  /* flags */
  /*ot->flag = OPTYPE_REGISTER;*/

  /* properties */
  static const EnumPropertyItem clog_filter_items[] = {
      {CLOG_FILTER_FUNCTION, "FILTER_FUNCTION", 0, "", ""},
      {CLOG_FILTER_FILE, "FILTER_FILE", 0, "", ""},
      {CLOG_FILTER_LINE, "FILTER_LINE", 0, "", ""},
      {CLOG_FILTER_LOG_TYPE, "FILTER_LOG_TYPE", 0, "", ""},
      {0, NULL, 0, NULL, NULL},
  };

  PropertyRNA *prop;
  prop = RNA_def_enum(ot->srna, "method", clog_filter_items, CLOG_FILTER_FILE, "Method", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}