#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <pango/pango.h>
#include "../entry.h"
#include "../log.h"
#include "../nelem.h"
#include "../xmalloc.h"

#undef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#undef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))

void entry_backend_pango_init(struct entry *entry, uint32_t *width, uint32_t *height)
{
	cairo_t *cr = entry->cairo[0].cr;

	/* Setup Pango. */
	log_debug("Creating Pango context.\n");
	PangoContext *context = pango_cairo_create_context(cr);

	log_debug("Creating Pango font description.\n");
	PangoFontDescription *font_description =
		pango_font_description_from_string(entry->font_name);
	pango_font_description_set_size(
			font_description,
			entry->font_size * PANGO_SCALE);
	pango_context_set_font_description(context, font_description);
	pango_font_description_free(font_description);

	entry->pango.layout = pango_layout_new(context);

	entry->pango.context = context;
}

void entry_backend_pango_destroy(struct entry *entry)
{
	g_object_unref(entry->pango.layout);
	g_object_unref(entry->pango.context);
}

static bool size_overflows(struct entry *entry, uint32_t width, uint32_t height)
{
	cairo_t *cr = entry->cairo[entry->index].cr;
	cairo_matrix_t mat;
	cairo_get_matrix(cr, &mat);
	if (entry->horizontal) {
		if (mat.x0 + width > entry->clip.x + entry->clip.width) {
			return true;
		}
	} else {
		if (mat.y0 + height > entry->clip.y + entry->clip.height) {
			return true;
		}
	}
	return false;
}

void entry_backend_pango_update(struct entry *entry)
{
	cairo_t *cr = entry->cairo[entry->index].cr;
	PangoLayout *layout = entry->pango.layout;

	cairo_save(cr);
	struct color color = entry->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

	/* Render the prompt */
	pango_layout_set_text(layout, entry->prompt_text, -1);
	pango_cairo_update_layout(cr, layout);
	pango_cairo_show_layout(cr, layout);

	int width;
	int height;
	pango_layout_get_pixel_size(entry->pango.layout, &width, NULL);
	cairo_translate(cr, width, 0);


	/* Render the entry text */
	pango_layout_set_text(layout, entry->input_mb, -1);
	pango_cairo_update_layout(cr, layout);
	pango_cairo_show_layout(cr, layout);
	pango_layout_get_size(layout, &width, &height);
	width = MAX(width, (int)entry->input_width * PANGO_SCALE);

	uint32_t num_results;
	if (entry->num_results == 0) {
		num_results = entry->results.count;
	} else {
		num_results = MIN(entry->num_results, entry->results.count);
	}
	/* Render our results */
	size_t i;
	for (i = 0; i < num_results; i++) {
		if (entry->horizontal) {
			cairo_translate(cr, (int)(width / PANGO_SCALE) + entry->result_spacing, 0);
		} else {
			cairo_translate(cr, 0, (int)(height / PANGO_SCALE) + entry->result_spacing);
		}
		if (entry->num_results == 0) {
			if (size_overflows(entry, 0, 0)) {
				break;
			}
		} else if (i >= entry->num_results) {
			break;
		}
		size_t index = i + entry->first_result;
		/*
		 * We may be on the last page, which could have fewer results
		 * than expected, so check and break if necessary.
		 */
		if (index >= entry->results.count) {
			break;
		}

		const char *str;
		if (i < entry->results.count) {
			str = entry->results.buf[index].string;
		} else {
			str = "";
		}
		if (i != entry->selection) {
			pango_layout_set_text(layout, str, -1);
			pango_cairo_update_layout(cr, layout);

			if (entry->num_results > 0) {
				pango_cairo_show_layout(cr, layout);
				pango_layout_get_size(layout, &width, &height);
			} else if (!entry->horizontal) {
				if (size_overflows(entry, 0, height / PANGO_SCALE)) {
					entry->num_results_drawn = i;
					break;
				} else {
					pango_cairo_show_layout(cr, layout);
					pango_layout_get_size(layout, &width, &height);
				}
			} else {
				cairo_push_group(cr);
				pango_cairo_show_layout(cr, layout);
				pango_layout_get_size(layout, &width, &height);
				cairo_pattern_t *group = cairo_pop_group(cr);
				if (size_overflows(entry, width / PANGO_SCALE, 0)) {
					entry->num_results_drawn = i;
					cairo_pattern_destroy(group);
					break;
				} else {
					cairo_save(cr);
					cairo_set_source(cr, group);
					cairo_paint(cr);
					cairo_restore(cr);
					cairo_pattern_destroy(group);
				}
			}
		} else {
			ssize_t prematch_len = -1;
			size_t match_len = entry->input_mb_length;
			int32_t subwidth;
			if (entry->input_mb_length > 0 && entry->selection_highlight_color.a != 0) {
				char *match_pos = strcasestr(str, entry->input_mb);
				if (match_pos != NULL) {
					prematch_len = (match_pos - str);
				}
			}

			cairo_push_group(cr);
			color = entry->selection_foreground_color;
			cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

			pango_layout_set_text(layout, str, prematch_len);
			pango_cairo_update_layout(cr, layout);
			pango_cairo_show_layout(cr, layout);
			pango_layout_get_size(layout, &subwidth, &height);
			width = subwidth;

			if (prematch_len != -1) {
				cairo_translate(cr, (int)(subwidth / PANGO_SCALE), 0);
				color = entry->selection_highlight_color;
				cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
				pango_layout_set_text(layout, &str[prematch_len], match_len);
				pango_cairo_update_layout(cr, layout);
				pango_cairo_show_layout(cr, layout);
				pango_layout_get_size(layout, &subwidth, &height);
				width += subwidth;

				cairo_translate(cr, (int)(subwidth / PANGO_SCALE), 0);
				color = entry->selection_foreground_color;
				cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
				pango_layout_set_text(layout, &str[prematch_len + match_len], -1);
				pango_cairo_update_layout(cr, layout);
				pango_cairo_show_layout(cr, layout);
				pango_layout_get_size(layout, &subwidth, &height);
				width += subwidth;

			}

			cairo_pop_group_to_source(cr);
			cairo_save(cr);
			color = entry->selection_background_color;
			cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
			int32_t pad = entry->selection_background_padding;
			if (pad < 0) {
				pad = entry->clip.width;
			}
			cairo_translate(cr, -pad, 0);
			cairo_rectangle(cr, 0, 0, (int)(width / PANGO_SCALE) + pad * 2, (int)(height / PANGO_SCALE));
			cairo_translate(cr, pad, 0);
			cairo_fill(cr);
			cairo_restore(cr);
			cairo_paint(cr);
			color = entry->foreground_color;
			cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
		}
	}
	entry->num_results_drawn = i;
	log_debug("Drew %zu results.\n", i);

	cairo_restore(cr);
}
