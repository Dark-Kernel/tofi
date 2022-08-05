#include <cairo/cairo.h>
#include <math.h>
#include <wchar.h>
#include <unistd.h>
#include "entry.h"
#include "log.h"
#include "nelem.h"

#undef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static void rounded_rectangle(cairo_t *cr, uint32_t width, uint32_t height, uint32_t r)
{
	cairo_new_path(cr);

	/* Top-left */
	cairo_arc(cr, r, r, r, -M_PI, -M_PI_2);

	/* Top-right */
	cairo_arc(cr, width - r, r, r, -M_PI_2, 0);

	/* Bottom-right */
	cairo_arc(cr, width - r, height - r, r, 0, M_PI_2);

	/* Bottom-left */
	cairo_arc(cr, r, height - r, r, M_PI_2, M_PI);

	cairo_close_path(cr);
}

void entry_init(struct entry *entry, uint8_t *restrict buffer, uint32_t width, uint32_t height)
{
	entry->image.width = width;
	entry->image.height = height;

	/*
	 * Create the cairo surfaces and contexts we'll be using.
	 *
	 * In order to avoid an unnecessary copy when passing the image to the
	 * Wayland server, we accept a pointer to the mmap-ed file that our
	 * Wayland buffers are created from. This is assumed to be
	 * (width * height * (sizeof(uint32_t) == 4) * 2) bytes,
	 * to allow for double buffering.
	 */
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
			buffer,
			CAIRO_FORMAT_ARGB32,
			width,
			height,
			width * sizeof(uint32_t)
			);
	cairo_t *cr = cairo_create(surface);

	entry->cairo[0].surface = surface;
	entry->cairo[0].cr = cr;

	entry->cairo[1].surface = cairo_image_surface_create_for_data(
			&buffer[width * height * sizeof(uint32_t)],
			CAIRO_FORMAT_ARGB32,
			width,
			height,
			width * sizeof(uint32_t)
			);
	entry->cairo[1].cr = cairo_create(entry->cairo[1].surface);


	/* Draw the background */
	struct color color = entry->background_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);

	/* Draw the border with outlines */
	cairo_set_line_width(cr, 4 * entry->outline_width + 2 * entry->border_width);
	rounded_rectangle(cr, width, height, entry->corner_radius);

	color = entry->outline_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_stroke_preserve(cr);

	color = entry->border_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_set_line_width(cr, 2 * entry->outline_width + 2 * entry->border_width);
	cairo_stroke_preserve(cr);

	color = entry->outline_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_set_line_width(cr, 2 * entry->outline_width);
	cairo_stroke_preserve(cr);

	/* Clear the overdrawn bits outside of the rounded corners */
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
	cairo_save(cr);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_fill(cr);
	cairo_restore(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);


	/* Move and clip following draws to be within this outline + padding */
	double dx = 2.0 * entry->outline_width + entry->border_width;
	cairo_translate(cr, dx + entry->padding_left, dx + entry->padding_top);
	width -= 2 * dx + entry->padding_left + entry->padding_right;
	height -= 2 * dx + entry->padding_top + entry->padding_bottom;

	/* Account for rounded corners */
	double inner_radius = (double)entry->corner_radius - dx;
	inner_radius = MAX(inner_radius, 0);

	dx = ceil(inner_radius * (1.0 - 1.0 / M_SQRT2));
	cairo_translate(cr, dx, dx);
	width -= 2 * dx;
	height -= 2 * dx;
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_clip(cr);

	/* Setup the backend. */
	if (access(entry->font_name, R_OK) != 0) {
		/*
		 * We've been given a font name rather than path,
		 * so fallback to Pango
		 */
		entry->use_pango = true;
	}
	if (entry->use_pango) {
		entry_backend_pango_init(entry, &width, &height);
	} else {
		entry_backend_harfbuzz_init(entry, &width, &height);
	}

	/* Store the clip rectangle width and height. */
	cairo_matrix_t mat;
	cairo_get_matrix(cr, &mat);
	entry->clip = (struct rectangle) {
		.x = mat.x0,
		.y = mat.y0,
		.width = width,
		.height = height
	};

	entry->cairo[0].damage_list = rect_vec_create();
	entry->cairo[1].damage_list = rect_vec_create();

	/*
	 * Perform an initial render of the text.
	 * This is done here rather than by calling entry_update to avoid the
	 * unnecessary cairo_paint() of the background for the first frame,
	 * which can be slow for large (e.g. fullscreen) windows.
	 */
	log_debug("Initial text render.\n");
	if (entry->use_pango) {
		entry_backend_pango_update(entry);
	} else {
		entry_backend_harfbuzz_update(entry);
	}
	entry->index = !entry->index;

	/*
	 * To avoid performing all this drawing twice, we take a small
	 * shortcut. After performing all the drawing as normal on our first
	 * Cairo context, we can copy over just the important state (the
	 * transformation matrix and clip rectangle) and perform a memcpy()
	 * to initialise the other context.
	 *
	 * This memcpy can pretty expensive however, and isn't needed until
	 * we need to draw our second buffer (i.e. when the user presses a
	 * key). In order to minimise startup time, the memcpy() isn't
	 * performed here, but instead happens later, just after the first
	 * frame has been displayed on screen (and while the user is unlikely
	 * to press another key for the <10ms it takes to memcpy).
	 */
	cairo_set_matrix(entry->cairo[1].cr, &mat);
	cairo_rectangle(entry->cairo[1].cr, 0, 0, width, height);
	cairo_clip(entry->cairo[1].cr);
}

void entry_destroy(struct entry *entry)
{
	if (entry->use_pango) {
		entry_backend_pango_destroy(entry);
	} else {
		entry_backend_harfbuzz_destroy(entry);
	}
	rect_vec_destroy(&entry->cairo[0].damage_list);
	rect_vec_destroy(&entry->cairo[1].damage_list);
	cairo_destroy(entry->cairo[0].cr);
	cairo_destroy(entry->cairo[1].cr);
	cairo_surface_destroy(entry->cairo[0].surface);
	cairo_surface_destroy(entry->cairo[1].surface);
}

void entry_update(struct entry *entry)
{
	log_debug("Start rendering entry.\n");
	cairo_t *cr = entry->cairo[entry->index].cr;
	struct rect_vec *damage_list = &entry->cairo[entry->index].damage_list;

	/* Clear the image. */
	struct color color = entry->background_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_matrix_t mat;
	cairo_get_matrix(cr, &mat);
	cairo_translate(cr, -mat.x0, -mat.y0);
	for (size_t i = 0; i < damage_list->count; i++) {
		struct rectangle rect = damage_list->buf[i];
		cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
		cairo_fill(cr);
	}
	rect_vec_clear(damage_list);
	cairo_restore(cr);

	/* Draw our text. */
	if (entry->use_pango) {
		entry_backend_pango_update(entry);
	} else {
		entry_backend_harfbuzz_update(entry);
	}

	//cairo_set_source_rgba(cr, 1, color.g, color.b, color.a);
	//cairo_save(cr);
	//cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	//cairo_get_matrix(cr, &mat);
	//cairo_translate(cr, -mat.x0, -mat.y0);
	//cairo_set_line_width(cr, 1);
	//cairo_set_source_rgba(cr, 1, color.g, color.b, color.a);
	//for (size_t i = 0; i < damage_list->count; i++) {
	//	struct rectangle rect = damage_list->buf[i];
	//	cairo_rectangle(cr, rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2);
	//	cairo_stroke(cr);
	//}
	//cairo_restore(cr);

	log_debug("Finish rendering entry.\n");

	entry->index = !entry->index;
}
