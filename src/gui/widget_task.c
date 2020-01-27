﻿/*
 * widget_task.c -- LCUI widget task module.
 *
 * Copyright (c) 2018-2019, Liu chao <lc-soft@live.cn> All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of LCUI nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <LCUI_Build.h>
#include <LCUI/LCUI.h>
#include <LCUI/gui/widget.h>
#include <LCUI/gui/metrics.h>

#define MEMCMP(A, B) memcmp(A, B, sizeof(*(A)))

typedef struct LCUI_WidgetTaskContextRec_ *LCUI_WidgetTaskContext;
typedef struct LCUI_WidgetLayoutTaskRec_ *LCUI_WidgetLayoutTask;

/** for check widget difference */
typedef struct LCUI_WidgetTaskDiffRec_ {
	int z_index;
	int display;
	float left;
	float right;
	float top;
	float bottom;
	float width;
	float height;
	float opacity;
	LCUI_BOOL visible;
	LCUI_Rect2F margin;
	LCUI_Rect2F padding;
	LCUI_StyleValue position;
	LCUI_BorderStyle border;
	LCUI_BoxShadowStyle shadow;
	LCUI_BackgroundStyle background;
	LCUI_WidgetBoxModelRec box;
	LCUI_FlexBoxLayoutStyle flex;

	int invalid_box;
	LCUI_BOOL can_render;
	LCUI_BOOL sync_props_to_surface;
	LCUI_BOOL should_add_invalid_area;
} LCUI_WidgetTaskDiffRec, *LCUI_WidgetTaskDiff;

typedef struct LCUI_WidgetLayoutTaskRec_ {
	LCUI_Widget widget;
	LinkedList children;
	LinkedListNode node;
	LCUI_WidgetLayoutTask parent;
	LCUI_WidgetLayoutContextRec ctx;
} LCUI_WidgetLayoutTaskRec;

typedef struct LCUI_WidgetTaskContextRec_ {
	unsigned style_hash;
	Dict *style_cache;
	LCUI_WidgetTaskDiffRec diff;
	LCUI_WidgetTaskContext parent;
	LCUI_WidgetTasksProfile profile;
} LCUI_WidgetTaskContextRec;

static struct WidgetTaskModule {
	unsigned max_updates_per_frame;
	DictType style_cache_dict;
	LCUI_MetricsRec metrics;
	LCUI_BOOL refresh_all;
	LCUI_WidgetFunction handlers[LCUI_WTASK_TOTAL_NUM];
} self;

static size_t Widget_UpdateWithContext(LCUI_Widget w,
				       LCUI_WidgetTaskContext ctx);

static unsigned int IntKeyDict_HashFunction(const void *key)
{
	return Dict_IdentityHashFunction(*(unsigned int *)key);
}

static int IntKeyDict_KeyCompare(void *privdata, const void *key1,
				 const void *key2)
{
	return *(unsigned int *)key1 == *(unsigned int *)key2;
}

static void IntKeyDict_KeyDestructor(void *privdata, void *key)
{
	free(key);
}

static void *IntKeyDict_KeyDup(void *privdata, const void *key)
{
	unsigned int *newkey = malloc(sizeof(unsigned int));
	*newkey = *(unsigned int *)key;
	return newkey;
}

static void StyleSheetCacheDestructor(void *privdata, void *val)
{
	StyleSheet_Delete(val);
}

static void InitStylesheetCacheDict(void)
{
	DictType *dt = &self.style_cache_dict;

	dt->valDup = NULL;
	dt->keyDup = IntKeyDict_KeyDup;
	dt->keyCompare = IntKeyDict_KeyCompare;
	dt->hashFunction = IntKeyDict_HashFunction;
	dt->keyDestructor = IntKeyDict_KeyDestructor;
	dt->valDestructor = StyleSheetCacheDestructor;
	dt->keyDestructor = IntKeyDict_KeyDestructor;
}

static void Widget_OnRefreshStyle(LCUI_Widget w)
{
	int i;

	Widget_ExecUpdateStyle(w, TRUE);
	for (i = LCUI_WTASK_UPDATE_STYLE + 1; i < LCUI_WTASK_REFLOW; ++i) {
		w->task.states[i] = TRUE;
	}
	w->task.states[LCUI_WTASK_UPDATE_STYLE] = FALSE;
}

static void Widget_OnUpdateStyle(LCUI_Widget w)
{
	Widget_ExecUpdateStyle(w, FALSE);
}

static void Widget_OnSetTitle(LCUI_Widget w)
{
	Widget_PostSurfaceEvent(w, LCUI_WEVENT_TITLE, TRUE);
}

static void Widget_OnUpdateContentBox(LCUI_Widget w)
{
	LinkedListNode *node;

	for (LinkedList_Each(node, &w->children)) {
		Widget_AddTask(node->data, LCUI_WTASK_POSITION);
		Widget_AddTask(node->data, LCUI_WTASK_RESIZE);
	}
}

void Widget_UpdateTaskStatus(LCUI_Widget widget)
{
	int i;

	for (i = 0; i < LCUI_WTASK_TOTAL_NUM; ++i) {
		if (widget->task.states[i]) {
			break;
		}
	}
	if (i >= LCUI_WTASK_TOTAL_NUM) {
		return;
	}
	widget->task.for_self = TRUE;
	while (widget && !widget->task.for_children) {
		widget->task.for_children = TRUE;
		widget = widget->parent;
	}
}

void Widget_AddTaskForChildren(LCUI_Widget widget, int task)
{
	LCUI_Widget child;
	LinkedListNode *node;

	widget->task.for_children = TRUE;
	for (LinkedList_Each(node, &widget->children)) {
		child = node->data;
		Widget_AddTask(child, task);
		Widget_AddTaskForChildren(child, task);
	}
}

void Widget_AddTask(LCUI_Widget widget, int task)
{
	if (widget->state == LCUI_WSTATE_DELETED) {
		return;
	}
	DEBUG_MSG("[%lu] %s, %d\n", widget->index, widget->type, task);
	widget->task.for_self = TRUE;
	widget->task.states[task] = TRUE;
	widget = widget->parent;
	/* 向没有标记的父级部件添加标记 */
	while (widget && !widget->task.for_children) {
		widget->task.for_children = TRUE;
		widget = widget->parent;
	}
}

void LCUIWidget_InitTasks(void)
{
#define SetHandler(NAME, HANDLER) self.handlers[LCUI_WTASK_##NAME] = HANDLER
	SetHandler(VISIBLE, Widget_ComputeVisibilityStyle);
	SetHandler(POSITION, Widget_ComputePositionStyle);
	SetHandler(RESIZE, Widget_ComputeSizeStyle);
	SetHandler(SHADOW, Widget_ComputeBoxShadowStyle);
	SetHandler(BORDER, Widget_ComputeBorderStyle);
	SetHandler(OPACITY, Widget_ComputeOpacityStyle);
	SetHandler(MARGIN, Widget_ComputeMarginStyle);
	SetHandler(PADDING, Widget_ComputePaddingStyle);
	SetHandler(BACKGROUND, Widget_ComputeBackgroundStyle);
	SetHandler(ZINDEX, Widget_ComputeZIndexStyle);
	SetHandler(DISPLAY, Widget_ComputeDisplayStyle);
	SetHandler(FLEX, Widget_ComputeFlexBoxStyle);
	SetHandler(PROPS, Widget_ComputeProperties);
	SetHandler(UPDATE_STYLE, Widget_OnUpdateStyle);
	SetHandler(REFRESH_STYLE, Widget_OnRefreshStyle);
	SetHandler(TITLE, Widget_OnSetTitle);
	self.handlers[LCUI_WTASK_REFLOW] = NULL;
	InitStylesheetCacheDict();
	self.refresh_all = TRUE;
	self.max_updates_per_frame = 4;
}

void LCUIWidget_FreeTasks(void)
{
	LCUIWidget_ClearTrash();
}

static void Widget_InitDiff(LCUI_Widget w, LCUI_WidgetTaskContext ctx)
{
	ctx->diff.can_render = TRUE;
	ctx->diff.invalid_box = self.refresh_all ? SV_GRAPH_BOX : 0;
	ctx->diff.should_add_invalid_area = FALSE;
	if (ctx->parent) {
		if (!ctx->parent->diff.can_render) {
			ctx->diff.can_render = FALSE;
			return;
		}
		if (ctx->parent->diff.invalid_box >= SV_PADDING_BOX) {
			ctx->diff.invalid_box = SV_GRAPH_BOX;
			return;
		}
	}
	if (w->state < LCUI_WSTATE_LAYOUTED) {
		ctx->diff.invalid_box = SV_GRAPH_BOX;
	}
	ctx->diff.should_add_invalid_area = TRUE;
}

static void Widget_BeginDiff(LCUI_Widget w, LCUI_WidgetTaskContext ctx)
{
	const LCUI_WidgetStyle *style = &w->computed_style;

	if (self.refresh_all) {
		memset(&ctx->diff, 0, sizeof(ctx->diff));
		Widget_InitDiff(w, ctx);
	} else {
		ctx->diff.left = w->computed_style.left;
		ctx->diff.right = w->computed_style.right;
		ctx->diff.top = w->computed_style.top;
		ctx->diff.bottom = w->computed_style.bottom;
		ctx->diff.width = w->width;
		ctx->diff.height = w->height;
		ctx->diff.margin = w->margin;
		ctx->diff.padding = w->padding;
		ctx->diff.display = style->display;
		ctx->diff.z_index = style->z_index;
		ctx->diff.visible = style->visible;
		ctx->diff.opacity = style->opacity;
		ctx->diff.position = style->position;
		ctx->diff.shadow = style->shadow;
		ctx->diff.border = style->border;
		ctx->diff.background = style->background;
		ctx->diff.flex = style->flex;
		ctx->diff.box = w->box;
	}
}

INLINE void Widget_AddReflowTask(LCUI_Widget w)
{
	if (w) {
		if (w->parent && Widget_IsFlexLayoutStyleWorks(w)) {
			Widget_AddTask(w->parent, LCUI_WTASK_REFLOW);
		}
		Widget_AddTask(w, LCUI_WTASK_REFLOW);
	}
}

static int Widget_EndDiff(LCUI_Widget w, LCUI_WidgetTaskContext ctx)
{
	LCUI_RectF rect;
	const LCUI_WidgetTaskDiff diff = &ctx->diff;
	const LCUI_WidgetStyle *style = &w->computed_style;

	if (!diff->can_render) {
		return 0;
	}
	diff->can_render = style->visible;
	if (style->visible != diff->visible) {
		diff->invalid_box = SV_GRAPH_BOX;
		if (style->visible) {
			Widget_PostSurfaceEvent(w, LCUI_WEVENT_SHOW, TRUE);
		} else {
			Widget_PostSurfaceEvent(w, LCUI_WEVENT_HIDE, TRUE);
		}
	}

	/* check layout related property changes */

	Widget_UpdateBoxSize(w);
	Widget_UpdateBoxPosition(w);
	if (MEMCMP(&diff->box.padding, &w->box.padding)) {
		diff->invalid_box = SV_GRAPH_BOX;
		Widget_OnUpdateContentBox(w);
		Widget_AddReflowTask(w);
	} else if (MEMCMP(&diff->box.outer, &w->box.outer)) {
		diff->invalid_box = SV_GRAPH_BOX;
		Widget_AddReflowTask(w->parent);
	} else if (MEMCMP(&diff->box.canvas, &w->box.canvas)) {
		diff->invalid_box = SV_GRAPH_BOX;
	}
	if (Widget_IsFlexLayoutStyleWorks(w)) {
		if (diff->flex.wrap != style->flex.wrap ||
		    diff->flex.direction != style->flex.direction ||
		    diff->flex.justify_content != style->flex.justify_content ||
		    diff->flex.align_content != style->flex.align_content ||
		    diff->flex.align_items != style->flex.align_items) {
			Widget_AddReflowTask(w);
		}
		if (diff->flex.grow != style->flex.grow ||
		    diff->flex.shrink != style->flex.shrink ||
		    diff->flex.basis != style->flex.basis) {
			Widget_AddReflowTask(w->parent);
		}
	}
	if (diff->display != style->display) {
		diff->invalid_box = SV_GRAPH_BOX;
		if (style->position != SV_ABSOLUTE) {
			Widget_AddReflowTask(w->parent);
		}
		if (style->display != SV_NONE) {
			Widget_AddReflowTask(w);
		}
	} else if (diff->position != style->position) {
		diff->invalid_box = SV_GRAPH_BOX;
		if (diff->position == SV_ABSOLUTE ||
		    style->position == SV_ABSOLUTE) {
			Widget_AddReflowTask(w);
		}
		Widget_AddReflowTask(w->parent);
	}

	/* check repaint related property changes */

	if (!diff->should_add_invalid_area) {
		return 0;
	}
	if (diff->invalid_box == SV_GRAPH_BOX) {
	} else if (diff->z_index != style->z_index &&
		   style->position != SV_STATIC) {
		diff->invalid_box = SV_GRAPH_BOX;
	} else if (MEMCMP(&diff->shadow, &style->shadow)) {
		diff->invalid_box = SV_GRAPH_BOX;
	} else if (diff->invalid_box == SV_BORDER_BOX) {
	} else if (MEMCMP(&diff->border, &style->border)) {
		diff->invalid_box = SV_BORDER_BOX;
	} else if (MEMCMP(&diff->background, &style->background)) {
		diff->invalid_box = SV_BORDER_BOX;
	} else {
		return 0;
	}

	/* invalid area will be processed after reflow */
	if (w->task.states[LCUI_WTASK_REFLOW]) {
		return 0;
	}
	if (diff->invalid_box >= SV_BORDER_BOX) {
		Widget_UpdateCanvasBox(w);
	}
	if (!w->parent) {
		Widget_InvalidateArea(w, NULL, diff->invalid_box);
		return 1;
	}
	if (!LCUIRectF_IsCoverRect(&diff->box.canvas, &w->box.canvas)) {
		Widget_InvalidateArea(w->parent, &diff->box.canvas,
				      SV_PADDING_BOX);
		Widget_InvalidateArea(w, NULL, diff->invalid_box);
		return 1;
	}
	LCUIRectF_MergeRect(&rect, &diff->box.canvas, &w->box.canvas);
	Widget_InvalidateArea(w->parent, &rect, SV_PADDING_BOX);
	return 1;
}

LCUI_WidgetTaskContext Widget_BeginUpdate(LCUI_Widget w,
					  LCUI_WidgetTaskContext ctx)
{
	unsigned hash;
	LCUI_Selector selector;
	LCUI_StyleSheet style;
	LCUI_WidgetRulesData data;
	LCUI_CachedStyleSheet inherited_style;
	LCUI_WidgetTaskContext self_ctx;
	LCUI_WidgetTaskContext parent_ctx;

	self_ctx = malloc(sizeof(LCUI_WidgetTaskContextRec));
	if (!self_ctx) {
		return NULL;
	}
	self_ctx->parent = ctx;
	self_ctx->style_cache = NULL;
	for (parent_ctx = ctx; parent_ctx; parent_ctx = parent_ctx->parent) {
		if (parent_ctx->style_cache) {
			self_ctx->style_cache = parent_ctx->style_cache;
			self_ctx->style_hash = parent_ctx->style_hash;
			break;
		}
	}
	if (ctx && ctx->profile) {
		self_ctx->profile = ctx->profile;
	} else {
		self_ctx->profile = NULL;
	}
	if (w->hash && w->task.states[LCUI_WTASK_REFRESH_STYLE]) {
		Widget_GenerateSelfHash(w);
	}
	if (!self_ctx->style_cache && w->rules &&
	    w->rules->cache_children_style) {
		data = (LCUI_WidgetRulesData)w->rules;
		if (!data->style_cache) {
			data->style_cache =
			    Dict_Create(&self.style_cache_dict, NULL);
		}
		Widget_GenerateSelfHash(w);
		self_ctx->style_hash = w->hash;
		self_ctx->style_cache = data->style_cache;
	}
	inherited_style = w->inherited_style;
	if (self_ctx->style_cache && w->hash) {
		hash = self_ctx->style_hash;
		hash = ((hash << 5) + hash) + w->hash;
		style = Dict_FetchValue(self_ctx->style_cache, &hash);
		if (!style) {
			style = StyleSheet();
			selector = Widget_GetSelector(w);
			LCUI_GetStyleSheet(selector, style);
			Dict_Add(self_ctx->style_cache, &hash, style);
			Selector_Delete(selector);
		}
		w->inherited_style = style;
	} else {
		selector = Widget_GetSelector(w);
		w->inherited_style = LCUI_GetCachedStyleSheet(selector);
		Selector_Delete(selector);
	}
	if (w->inherited_style != inherited_style) {
		Widget_AddTask(w, LCUI_WTASK_REFRESH_STYLE);
	}
	return self_ctx;
}

void Widget_EndUpdate(LCUI_WidgetTaskContext ctx)
{
	ctx->style_cache = NULL;
	ctx->parent = NULL;
	free(ctx);
}

static size_t Widget_UpdateVisibleChildren(LCUI_Widget w,
					   LCUI_WidgetTaskContext ctx)
{
	size_t total = 0, count;
	LCUI_BOOL found = FALSE;
	LCUI_RectF rect, visible_rect;
	LCUI_Widget child, parent;
	LinkedListNode *node, *next;

	rect = w->box.padding;
	if (rect.width < 1 && Widget_HasAutoStyle(w, key_width)) {
		rect.width = w->parent->box.padding.width;
	}
	if (rect.height < 1 && Widget_HasAutoStyle(w, key_height)) {
		rect.height = w->parent->box.padding.height;
	}
	for (child = w, parent = w->parent; parent;
	     child = parent, parent = parent->parent) {
		if (child == w) {
			continue;
		}
		rect.x += child->box.padding.x;
		rect.y += child->box.padding.y;
		LCUIRectF_ValidateArea(&rect, parent->box.padding.width,
				       parent->box.padding.height);
	}
	visible_rect = rect;
	rect = w->box.padding;
	Widget_GetOffset(w, NULL, &rect.x, &rect.y);
	if (!LCUIRectF_GetOverlayRect(&visible_rect, &rect, &visible_rect)) {
		return 0;
	}
	visible_rect.x -= w->box.padding.x;
	visible_rect.y -= w->box.padding.y;
	for (node = w->children.head.next; node; node = next) {
		child = node->data;
		next = node->next;
		if (!LCUIRectF_GetOverlayRect(&visible_rect, &child->box.border,
					      &rect)) {
			if (found) {
				break;
			}
			continue;
		}
		found = TRUE;
		count = Widget_UpdateWithContext(child, ctx);
		if (child->task.for_self || child->task.for_children) {
			w->task.for_children = TRUE;
		}
		total += count;
		node = next;
	}
	return total;
}

static size_t Widget_UpdateChildren(LCUI_Widget w, LCUI_WidgetTaskContext ctx)
{
	clock_t msec = 0;
	LCUI_Widget child;
	LCUI_WidgetRulesData data;
	LinkedListNode *node, *next;
	size_t total = 0, update_count = 0, count;

	if (!w->task.for_children) {
		return 0;
	}
	data = (LCUI_WidgetRulesData)w->rules;
	node = w->children.head.next;
	if (data) {
		msec = clock();
		if (data->rules.only_on_visible) {
			if (!Widget_InVisibleArea(w)) {
				DEBUG_MSG("%s %s: is not visible\n", w->type,
					  w->id);
				return 0;
			}
		}
		DEBUG_MSG("%s %s: is visible\n", w->type, w->id);
		if (data->rules.first_update_visible_children) {
			total += Widget_UpdateVisibleChildren(w, ctx);
			DEBUG_MSG("first update visible children "
				  "count: %zu\n",
				  total);
		}
	}
	if (!w->task.for_children) {
		return 0;
	}
	w->task.for_children = FALSE;
	while (node) {
		child = node->data;
		next = node->next;
		count = Widget_UpdateWithContext(child, ctx);
		if (child->task.for_self || child->task.for_children) {
			w->task.for_children = TRUE;
		}
		total += count;
		node = next;
		if (!data) {
			continue;
		}
		if (count > 0) {
			data->progress = max(child->index, data->progress);
			if (data->progress > w->children_show.length) {
				data->progress = child->index;
			}
			update_count += 1;
		}
		if (data->rules.max_update_children_count < 0) {
			continue;
		}
		if (data->rules.max_update_children_count > 0) {
			if (update_count >=
			    (size_t)data->rules.max_update_children_count) {
				w->task.for_children = TRUE;
				break;
			}
		}
		if (update_count < data->default_max_update_count) {
			continue;
		}
		w->task.for_children = TRUE;
		msec = (clock() - msec);
		if (msec < 1) {
			data->default_max_update_count += 128;
			continue;
		}
		data->default_max_update_count = update_count * CLOCKS_PER_SEC /
						 self.max_updates_per_frame /
						 LCUI_MAX_FRAMES_PER_SEC / msec;
		if (data->default_max_update_count < 1) {
			data->default_max_update_count = 32;
		}
		break;
	}
	if (data) {
		if (!w->task.for_children) {
			data->progress = w->children_show.length;
		}
		if (data->rules.on_update_progress) {
			data->rules.on_update_progress(w, data->progress);
		}
	}
	return total;
}

static void Widget_UpdateSelf(LCUI_Widget w, LCUI_WidgetTaskContext ctx)
{
	int i;
	LCUI_BOOL *states;

	Widget_BeginDiff(w, ctx);
	states = w->task.states;
	if (states[LCUI_WTASK_USER] && w->proto && w->proto->runtask) {
		states[LCUI_WTASK_USER] = FALSE;
		w->proto->runtask(w);
	}
	w->task.for_self = FALSE;
	for (i = 0; i < LCUI_WTASK_REFLOW; ++i) {
		if (states[i]) {
			states[i] = FALSE;
			if (self.handlers[i]) {
				self.handlers[i](w);
			}
		}
	}
	Widget_EndDiff(w, ctx);
	Widget_AddState(w, LCUI_WSTATE_UPDATED);
}

static void Widget_RunReflowTask(LCUI_Widget w, LCUI_WidgetTaskContext task)
{
	LCUI_WidgetLayoutContextRec ctx;

	ctx.container = w;
	ctx.box = task->diff.box;
	ctx.invalid_box = task->diff.invalid_box;
	ctx.should_add_invalid_area =
	    task->diff.can_render && task->diff.should_add_invalid_area;
	LCUIWidgetLayout_Reflow(&ctx);
}

static size_t Widget_UpdateWithContext(LCUI_Widget w,
				       LCUI_WidgetTaskContext ctx)
{
	size_t count = 0;
	LCUI_WidgetTaskContext self_ctx;

	if (!w->task.for_self && !w->task.for_children) {
		return 0;
	}
	self_ctx = Widget_BeginUpdate(w, ctx);
	Widget_InitDiff(w, self_ctx);
	if (w->task.for_self) {
		Widget_UpdateSelf(w, self_ctx);
	}
	if (w->task.for_children) {
		count += Widget_UpdateChildren(w, self_ctx);
	}
	Widget_SortChildrenShow(w);
	if (w->task.states[LCUI_WTASK_REFLOW]) {
		Widget_RunReflowTask(w, self_ctx);
		w->task.states[LCUI_WTASK_REFLOW] = FALSE;
	}
	Widget_EndUpdate(self_ctx);
	return count;
}

size_t Widget_Update(LCUI_Widget w)
{
	size_t count;
	LCUI_WidgetTaskContext ctx;

	ctx = Widget_BeginUpdate(w, NULL);
	Widget_InitDiff(w, ctx);
	count = Widget_UpdateWithContext(w, ctx);
	Widget_EndUpdate(ctx);
	return count;
}

size_t LCUIWidget_Update(void)
{
	unsigned i;
	size_t count;
	LCUI_Widget root;
	const LCUI_MetricsRec *metrics;

	metrics = LCUI_GetMetrics();
	if (memcmp(metrics, &self.metrics, sizeof(LCUI_MetricsRec))) {
		self.refresh_all = TRUE;
	}
	if (self.refresh_all) {
		LCUIWidget_RefreshStyle();
	}
	root = LCUIWidget_GetRoot();
	for (count = i = 0; i < 4; ++i) {
		count += Widget_Update(root);
	}
	root->state = LCUI_WSTATE_NORMAL;
	LCUIWidget_ClearTrash();
	self.metrics = *metrics;
	self.refresh_all = FALSE;
	return count;
}

void Widget_UpdateWithProfile(LCUI_Widget w, LCUI_WidgetTasksProfile profile)
{
	LCUI_WidgetTaskContext ctx;

	ctx = Widget_BeginUpdate(w, NULL);
	ctx->profile = profile;
	Widget_UpdateWithContext(w, ctx);
	Widget_EndUpdate(ctx);
}

void LCUIWidget_UpdateWithProfile(LCUI_WidgetTasksProfile profile)
{
	unsigned i;
	LCUI_Widget root;
	const LCUI_MetricsRec *metrics;

	profile->time = clock();
	metrics = LCUI_GetMetrics();
	if (memcmp(metrics, &self.metrics, sizeof(LCUI_MetricsRec))) {
		self.refresh_all = TRUE;
	}
	if (self.refresh_all) {
		LCUIWidget_RefreshStyle();
	}
	if (self.refresh_all) {
		LCUIWidget_RefreshStyle();
	}
	root = LCUIWidget_GetRoot();
	for (i = 0; i < 4; ++i) {
		Widget_UpdateWithProfile(root, profile);
	}
	root->state = LCUI_WSTATE_NORMAL;
	profile->time = clock() - profile->time;
	profile->destroy_time = clock();
	profile->destroy_count = LCUIWidget_ClearTrash();
	profile->destroy_time = clock() - profile->destroy_time;
}

void LCUIWidget_RefreshStyle(void)
{
	LCUI_Widget root = LCUIWidget_GetRoot();
	Widget_UpdateStyle(root, TRUE);
	Widget_AddTaskForChildren(root, LCUI_WTASK_REFRESH_STYLE);
}
