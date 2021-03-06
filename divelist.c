/* divelist.c */
/* this creates the UI for the dive list -
 * controlled through the following interfaces:
 *
 * void flush_divelist(struct dive *dive)
 * GtkWidget dive_list_create(void)
 * void dive_list_update_dives(void)
 * void update_dive_list_units(void)
 * void set_divelist_font(const char *font)
 * void mark_divelist_changed(int changed)
 * int unsaved_changes()
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "divelist.h"
#include "dive.h"
#include "display.h"
#include "display-gtk.h"

struct DiveList {
	GtkWidget    *tree_view;
	GtkWidget    *container_widget;
	GtkTreeStore *model, *listmodel, *treemodel;
	GtkTreeViewColumn *nr, *date, *stars, *depth, *duration, *location;
	GtkTreeViewColumn *temperature, *cylinder, *totalweight, *suit, *nitrox, *sac, *otu;
	int changed;
};

static struct DiveList dive_list;
GList *dive_trip_list;
gboolean autogroup = FALSE;

const char *tripflag_names[NUM_TRIPFLAGS] = { "TF_NONE", "NOTRIP", "INTRIP" };

/*
 * The dive list has the dive data in both string format (for showing)
 * and in "raw" format (for sorting purposes)
 */
enum {
	DIVE_INDEX = 0,
	DIVE_NR,		/* int: dive->nr */
	DIVE_DATE,		/* time_t: dive->when */
	DIVE_RATING,		/* int: 0-5 stars */
	DIVE_DEPTH,		/* int: dive->maxdepth in mm */
	DIVE_DURATION,		/* int: in seconds */
	DIVE_TEMPERATURE,	/* int: in mkelvin */
	DIVE_TOTALWEIGHT,	/* int: in grams */
	DIVE_SUIT,		/* "wet, 3mm" */
	DIVE_CYLINDER,
	DIVE_NITROX,		/* int: dummy */
	DIVE_SAC,		/* int: in ml/min */
	DIVE_OTU,		/* int: in OTUs */
	DIVE_LOCATION,		/* "2nd Cathedral, Lanai" */
	DIVELIST_COLUMNS
};

#ifdef DEBUG_MODEL
static gboolean dump_model_entry(GtkTreeModel *model, GtkTreePath *path,
				GtkTreeIter *iter, gpointer data)
{
	char *location;
	int idx, nr, duration;
	struct dive *dive;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_NR, &nr, DIVE_DURATION, &duration, DIVE_LOCATION, &location, -1);
	printf("entry #%d : nr %d duration %d location %s ", idx, nr, duration, location);
	dive = get_dive(idx);
	if (dive)
		printf("tripflag %d\n", dive->tripflag);
	else
		printf("without matching dive\n");

	free(location);

	return FALSE;
}

static void dump_model(GtkListStore *store)
{
	gtk_tree_model_foreach(GTK_TREE_MODEL(store), dump_model_entry, NULL);
}
#endif

#if DEBUG_SELECTION_TRACKING
void dump_selection(void)
{
	int i;
	struct dive *dive;

	printf("currently selected are %d dives:", amount_selected);
	for_each_dive(i, dive) {
		if (dive->selected)
			printf(" %d", i);
	}
	printf("\n");
}
#endif

/* when subsurface starts we want to have the last dive selected. So we simply
   walk to the first leaf (and skip the summary entries - which have negative
   DIVE_INDEX) */
static void first_leaf(GtkTreeModel *model, GtkTreeIter *iter, int *diveidx)
{
	GtkTreeIter parent;
	GtkTreePath *tpath;

	while (*diveidx < 0) {
		memcpy(&parent, iter, sizeof(parent));
		tpath = gtk_tree_model_get_path(model, &parent);
		if (!gtk_tree_model_iter_children(model, iter, &parent))
			/* we should never have a parent without child */
			return;
		if(!gtk_tree_view_row_expanded(GTK_TREE_VIEW(dive_list.tree_view), tpath))
			gtk_tree_view_expand_row(GTK_TREE_VIEW(dive_list.tree_view), tpath, FALSE);
		gtk_tree_model_get(GTK_TREE_MODEL(model), iter, DIVE_INDEX, diveidx, -1);
	}
}

/* make sure that if we expand a summary row that is selected, the children show
   up as selected, too */
void row_expanded_cb(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path, gpointer data)
{
	GtkTreeIter child;
	GtkTreeModel *model = GTK_TREE_MODEL(dive_list.model);
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));

	if (!gtk_tree_model_iter_children(model, &child, iter))
		return;

	do {
		int idx;
		struct dive *dive;

		gtk_tree_model_get(model, &child, DIVE_INDEX, &idx, -1);
		dive = get_dive(idx);

		if (dive->selected)
			gtk_tree_selection_select_iter(selection, &child);
		else
			gtk_tree_selection_unselect_iter(selection, &child);
	} while (gtk_tree_model_iter_next(model, &child));
}

static int selected_children(GtkTreeModel *model, GtkTreeIter *iter)
{
	GtkTreeIter child;

	if (!gtk_tree_model_iter_children(model, &child, iter))
		return FALSE;

	do {
		int idx;
		struct dive *dive;

		gtk_tree_model_get(model, &child, DIVE_INDEX, &idx, -1);
		dive = get_dive(idx);

		if (dive->selected)
			return TRUE;
	} while (gtk_tree_model_iter_next(model, &child));
	return FALSE;
}

/* Make sure that if we collapse a summary row with any selected children, the row
   shows up as selected too */
void row_collapsed_cb(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path, gpointer data)
{
	GtkTreeModel *model = GTK_TREE_MODEL(dive_list.model);
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));

	if (selected_children(model, iter))
		gtk_tree_selection_select_iter(selection, iter);
}

static GList *selection_changed = NULL;

/*
 * This is called _before_ the selection is changed, for every single entry;
 *
 * We simply create a list of all changed entries, and make sure that the
 * group entries go at the end of the list.
 */
gboolean modify_selection_cb(GtkTreeSelection *selection, GtkTreeModel *model,
			GtkTreePath *path, gboolean was_selected, gpointer userdata)
{
	GtkTreeIter iter, *p;

	if (!gtk_tree_model_get_iter(model, &iter, path))
		return TRUE;

	/* Add the group entries to the end */
	p = gtk_tree_iter_copy(&iter);
	if (gtk_tree_model_iter_has_child(model, p))
		selection_changed = g_list_append(selection_changed, p);
	else
		selection_changed = g_list_prepend(selection_changed, p);
	return TRUE;
}

static void select_dive(struct dive *dive, int selected)
{
	if (dive->selected != selected) {
		amount_selected += selected ? 1 : -1;
		dive->selected = selected;
	}
}

/*
 * This gets called when a dive group has changed selection.
 */
static void select_dive_group(GtkTreeModel *model, GtkTreeSelection *selection, GtkTreeIter *iter, int selected)
{
	int first = 1;
	GtkTreeIter child;

	if (selected == selected_children(model, iter))
		return;

	if (!gtk_tree_model_iter_children(model, &child, iter))
		return;

	do {
		int idx;
		struct dive *dive;

		gtk_tree_model_get(model, &child, DIVE_INDEX, &idx, -1);
		if (first && selected)
			selected_dive = idx;
		first = 0;
		dive = get_dive(idx);
		if (dive->selected == selected)
			continue;

		select_dive(dive, selected);
		if (selected)
			gtk_tree_selection_select_iter(selection, &child);
		else
			gtk_tree_selection_unselect_iter(selection, &child);
	} while (gtk_tree_model_iter_next(model, &child));
}

/*
 * This gets called _after_ the selections have changed, for each entry that
 * may have changed. Check if the gtk selection state matches our internal
 * selection state to verify.
 *
 * The group entries are at the end, this guarantees that we have handled
 * all the dives before we handle groups.
 */
static void check_selection_cb(GtkTreeIter *iter, GtkTreeSelection *selection)
{
	GtkTreeModel *model = GTK_TREE_MODEL(dive_list.model);
	struct dive *dive;
	int idx, gtk_selected;

	gtk_tree_model_get(model, iter,
		DIVE_INDEX, &idx,
		-1);
	dive = get_dive(idx);
	gtk_selected = gtk_tree_selection_iter_is_selected(selection, iter);
	if (idx < 0)
		select_dive_group(model, selection, iter, gtk_selected);
	else {
		select_dive(dive, gtk_selected);
		if (gtk_selected)
			selected_dive = idx;
	}
	gtk_tree_iter_free(iter);
}

/* this is called when gtk thinks that the selection has changed */
static void selection_cb(GtkTreeSelection *selection, GtkTreeModel *model)
{
	GList *changed = selection_changed;

	selection_changed = NULL;
	g_list_foreach(changed, (GFunc) check_selection_cb, selection);
	g_list_free(changed);
#if DEBUG_SELECTION_TRACKING
	dump_selection();
#endif

	process_selected_dives();
	repaint_dive();
}

const char *star_strings[] = {
	ZERO_STARS,
	ONE_STARS,
	TWO_STARS,
	THREE_STARS,
	FOUR_STARS,
	FIVE_STARS
};

static void star_data_func(GtkTreeViewColumn *col,
			   GtkCellRenderer *renderer,
			   GtkTreeModel *model,
			   GtkTreeIter *iter,
			   gpointer data)
{
	int nr_stars, idx;
	char buffer[40];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_RATING, &nr_stars, -1);
	if (idx < 0) {
		*buffer = '\0';
	} else {
		if (nr_stars < 0 || nr_stars > 5)
			nr_stars = 0;
		snprintf(buffer, sizeof(buffer), "%s", star_strings[nr_stars]);
	}
	g_object_set(renderer, "text", buffer, NULL);
}

static void date_data_func(GtkTreeViewColumn *col,
			   GtkCellRenderer *renderer,
			   GtkTreeModel *model,
			   GtkTreeIter *iter,
			   gpointer data)
{
	int val, idx, nr;
	struct tm *tm;
	time_t when;
	char buffer[40];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_DATE, &val, DIVE_NR, &nr, -1);

	/* 2038 problem */
	when = val;

	tm = gmtime(&when);
	if (idx < 0) {
		snprintf(buffer, sizeof(buffer),
			"Trip %s, %s %d, %d (%d dive%s)",
			weekday(tm->tm_wday),
			monthname(tm->tm_mon),
			tm->tm_mday, tm->tm_year + 1900,
			nr, nr > 1 ? "s" : "");
	} else {
		snprintf(buffer, sizeof(buffer),
			"%s, %s %d, %d %02d:%02d",
			weekday(tm->tm_wday),
			monthname(tm->tm_mon),
			tm->tm_mday, tm->tm_year + 1900,
			tm->tm_hour, tm->tm_min);
	}
	g_object_set(renderer, "text", buffer, NULL);
}

static void depth_data_func(GtkTreeViewColumn *col,
			    GtkCellRenderer *renderer,
			    GtkTreeModel *model,
			    GtkTreeIter *iter,
			    gpointer data)
{
	int depth, integer, frac, len, idx;
	char buffer[40];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_DEPTH, &depth, -1);

	if (idx < 0) {
		*buffer = '\0';
	} else {
		switch (output_units.length) {
		case METERS:
			/* To tenths of meters */
			depth = (depth + 49) / 100;
			integer = depth / 10;
			frac = depth % 10;
			if (integer < 20)
				break;
			if (frac >= 5)
				integer++;
			frac = -1;
			break;
		case FEET:
			integer = mm_to_feet(depth) + 0.5;
			frac = -1;
			break;
		default:
			return;
		}
		len = snprintf(buffer, sizeof(buffer), "%d", integer);
		if (frac >= 0)
			len += snprintf(buffer+len, sizeof(buffer)-len, ".%d", frac);
	}
	g_object_set(renderer, "text", buffer, NULL);
}

static void duration_data_func(GtkTreeViewColumn *col,
			       GtkCellRenderer *renderer,
			       GtkTreeModel *model,
			       GtkTreeIter *iter,
			       gpointer data)
{
	unsigned int sec;
	int idx;
	char buffer[16];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_DURATION, &sec, -1);
	if (idx < 0)
		*buffer = '\0';
	else
		snprintf(buffer, sizeof(buffer), "%d:%02d", sec / 60, sec % 60);

	g_object_set(renderer, "text", buffer, NULL);
}

static void temperature_data_func(GtkTreeViewColumn *col,
				  GtkCellRenderer *renderer,
				  GtkTreeModel *model,
				  GtkTreeIter *iter,
				  gpointer data)
{
	int value, idx;
	char buffer[80];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_TEMPERATURE, &value, -1);

	*buffer = 0;
	if (idx >= 0 && value) {
		double deg;
		switch (output_units.temperature) {
		case CELSIUS:
			deg = mkelvin_to_C(value);
			break;
		case FAHRENHEIT:
			deg = mkelvin_to_F(value);
			break;
		default:
			return;
		}
		snprintf(buffer, sizeof(buffer), "%.1f", deg);
	}

	g_object_set(renderer, "text", buffer, NULL);
}

static void nr_data_func(GtkTreeViewColumn *col,
			   GtkCellRenderer *renderer,
			   GtkTreeModel *model,
			   GtkTreeIter *iter,
			   gpointer data)
{
	int idx, nr;
	char buffer[40];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_NR, &nr, -1);
	if (idx < 0)
		*buffer = '\0';
	else
		snprintf(buffer, sizeof(buffer), "%d", nr);
	g_object_set(renderer, "text", buffer, NULL);
}

/*
 * Get "maximal" dive gas for a dive.
 * Rules:
 *  - Trimix trumps nitrox (highest He wins, O2 breaks ties)
 *  - Nitrox trumps air (even if hypoxic)
 * These are the same rules as the inter-dive sorting rules.
 */
static void get_dive_gas(struct dive *dive, int *o2_p, int *he_p, int *o2low_p)
{
	int i;
	int maxo2 = -1, maxhe = -1, mino2 = 1000;

	for (i = 0; i < MAX_CYLINDERS; i++) {
		cylinder_t *cyl = dive->cylinder + i;
		struct gasmix *mix = &cyl->gasmix;
		int o2 = mix->o2.permille;
		int he = mix->he.permille;

		if (cylinder_none(cyl))
			continue;
		if (!o2)
			o2 = AIR_PERMILLE;
		if (o2 < mino2)
			mino2 = o2;
		if (he > maxhe)
			goto newmax;
		if (he < maxhe)
			continue;
		if (o2 <= maxo2)
			continue;
newmax:
		maxhe = he;
		maxo2 = o2;
	}
	/* All air? Show/sort as "air"/zero */
	if (!maxhe && maxo2 == AIR_PERMILLE && mino2 == maxo2)
		maxo2 = mino2 = 0;
	*o2_p = maxo2;
	*he_p = maxhe;
	*o2low_p = mino2;
}

static int total_weight(struct dive *dive)
{
	int i, total_grams = 0;

	if (dive)
		for (i=0; i< MAX_WEIGHTSYSTEMS; i++)
			total_grams += dive->weightsystem[i].weight.grams;
	return total_grams;
}

static void weight_data_func(GtkTreeViewColumn *col,
			     GtkCellRenderer *renderer,
			     GtkTreeModel *model,
			     GtkTreeIter *iter,
			     gpointer data)
{
	int indx, decimals;
	double value;
	char buffer[80];
	struct dive *dive;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &indx, -1);
	dive = get_dive(indx);
	value = get_weight_units(total_weight(dive), &decimals, NULL);
	if (value == 0.0)
		*buffer = '\0';
	else
		snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);

	g_object_set(renderer, "text", buffer, NULL);
}

static gint nitrox_sort_func(GtkTreeModel *model,
	GtkTreeIter *iter_a,
	GtkTreeIter *iter_b,
	gpointer user_data)
{
	int index_a, index_b;
	struct dive *a, *b;
	int a_o2, b_o2;
	int a_he, b_he;
	int a_o2low, b_o2low;

	gtk_tree_model_get(model, iter_a, DIVE_INDEX, &index_a, -1);
	gtk_tree_model_get(model, iter_b, DIVE_INDEX, &index_b, -1);
	a = get_dive(index_a);
	b = get_dive(index_b);
	get_dive_gas(a, &a_o2, &a_he, &a_o2low);
	get_dive_gas(b, &b_o2, &b_he, &b_o2low);

	/* Sort by Helium first, O2 second */
	if (a_he == b_he) {
		if (a_o2 == b_o2)
			return a_o2low - b_o2low;
		return a_o2 - b_o2;
	}
	return a_he - b_he;
}

#define UTF8_ELLIPSIS "\xE2\x80\xA6"

static void nitrox_data_func(GtkTreeViewColumn *col,
			     GtkCellRenderer *renderer,
			     GtkTreeModel *model,
			     GtkTreeIter *iter,
			     gpointer data)
{
	int idx, o2, he, o2low;
	char buffer[80];
	struct dive *dive;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	if (idx < 0) {
		*buffer = '\0';
		goto exit;
	}
	dive = get_dive(idx);
	get_dive_gas(dive, &o2, &he, &o2low);
	o2 = (o2 + 5) / 10;
	he = (he + 5) / 10;
	o2low = (o2low + 5) / 10;

	if (he)
		snprintf(buffer, sizeof(buffer), "%d/%d", o2, he);
	else if (o2)
		if (o2 == o2low)
			snprintf(buffer, sizeof(buffer), "%d", o2);
		else
			snprintf(buffer, sizeof(buffer), "%d" UTF8_ELLIPSIS "%d", o2low, o2);
	else
		strcpy(buffer, "air");
exit:
	g_object_set(renderer, "text", buffer, NULL);
}

/* Render the SAC data (integer value of "ml / min") */
static void sac_data_func(GtkTreeViewColumn *col,
			  GtkCellRenderer *renderer,
			  GtkTreeModel *model,
			  GtkTreeIter *iter,
			  gpointer data)
{
	int value, idx;
	const char *fmt;
	char buffer[16];
	double sac;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_SAC, &value, -1);

	if (idx < 0 || !value) {
		*buffer = '\0';
		goto exit;
	}

	sac = value / 1000.0;
	switch (output_units.volume) {
	case LITER:
		fmt = "%4.1f";
		break;
	case CUFT:
		fmt = "%4.2f";
		sac = ml_to_cuft(sac * 1000);
		break;
	}
	snprintf(buffer, sizeof(buffer), fmt, sac);
exit:
	g_object_set(renderer, "text", buffer, NULL);
}

/* Render the OTU data (integer value of "OTU") */
static void otu_data_func(GtkTreeViewColumn *col,
			  GtkCellRenderer *renderer,
			  GtkTreeModel *model,
			  GtkTreeIter *iter,
			  gpointer data)
{
	int value, idx;
	char buffer[16];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_OTU, &value, -1);

	if (idx < 0 || !value)
		*buffer = '\0';
	else
		snprintf(buffer, sizeof(buffer), "%d", value);

	g_object_set(renderer, "text", buffer, NULL);
}

/* calculate OTU for a dive */
static int calculate_otu(struct dive *dive)
{
	int i;
	double otu = 0.0;

	for (i = 1; i < dive->samples; i++) {
		int t;
		double po2;
		struct sample *sample = dive->sample + i;
		struct sample *psample = sample - 1;
		t = sample->time.seconds - psample->time.seconds;
		int o2 = dive->cylinder[sample->cylinderindex].gasmix.o2.permille;
		if (!o2)
			o2 = AIR_PERMILLE;
		po2 = o2 / 1000.0 * (sample->depth.mm + 10000) / 10000.0;
		if (po2 >= 0.5)
			otu += pow(po2 - 0.5, 0.83) * t / 30.0;
	}
	return otu + 0.5;
}
/*
 * Return air usage (in liters).
 */
static double calculate_airuse(struct dive *dive)
{
	double airuse = 0;
	int i;

	for (i = 0; i < MAX_CYLINDERS; i++) {
		pressure_t start, end;
		cylinder_t *cyl = dive->cylinder + i;
		int size = cyl->type.size.mliter;
		double kilo_atm;

		if (!size)
			continue;

		start = cyl->start.mbar ? cyl->start : cyl->sample_start;
		end = cyl->end.mbar ? cyl->end : cyl->sample_end;
		kilo_atm = (to_ATM(start) - to_ATM(end)) / 1000.0;

		/* Liters of air at 1 atm == milliliters at 1k atm*/
		airuse += kilo_atm * size;
	}
	return airuse;
}

static int calculate_sac(struct dive *dive)
{
	double airuse, pressure, sac;
	int duration, i;

	airuse = calculate_airuse(dive);
	if (!airuse)
		return 0;
	if (!dive->duration.seconds)
		return 0;

	/* find and eliminate long surface intervals */
	duration = dive->duration.seconds;
	for (i = 0; i < dive->samples; i++) {
		if (dive->sample[i].depth.mm < 100) { /* less than 10cm */
			int end = i + 1;
			while (end < dive->samples && dive->sample[end].depth.mm < 100)
				end++;
			/* we only want the actual surface time during a dive */
			if (end < dive->samples) {
				end--;
				duration -= dive->sample[end].time.seconds -
						dive->sample[i].time.seconds;
				i = end + 1;
			}
		}
	}
	/* Mean pressure in atm: 1 atm per 10m */
	pressure = 1 + (dive->meandepth.mm / 10000.0);
	sac = airuse / pressure * 60 / duration;

	/* milliliters per minute.. */
	return sac * 1000;
}

void update_cylinder_related_info(struct dive *dive)
{
	if (dive != NULL) {
		dive->sac = calculate_sac(dive);
		dive->otu = calculate_otu(dive);
	}
}

static void get_string(char **str, const char *s)
{
	int len;
	char *n;

	if (!s)
		s = "";
	len = strlen(s);
	if (len > 60)
		len = 60;
	n = malloc(len+1);
	memcpy(n, s, len);
	n[len] = 0;
	*str = n;
}

static void get_location(struct dive *dive, char **str)
{
	get_string(str, dive->location);
}

static void get_cylinder(struct dive *dive, char **str)
{
	get_string(str, dive->cylinder[0].type.description);
}

static void get_suit(struct dive *dive, char **str)
{
	get_string(str, dive->suit);
}

/*
 * Set up anything that could have changed due to editing
 * of dive information; we need to do this for both models,
 * so we simply call set_one_dive again with the non-current model
 */
/* forward declaration for recursion */
static gboolean set_one_dive(GtkTreeModel *model,
			     GtkTreePath *path,
			     GtkTreeIter *iter,
			     gpointer data);

static void fill_one_dive(struct dive *dive,
			  GtkTreeModel *model,
			  GtkTreeIter *iter)
{
	char *location, *cylinder, *suit;
	GtkTreeStore *othermodel;

	get_cylinder(dive, &cylinder);
	get_location(dive, &location);
	get_suit(dive, &suit);

	gtk_tree_store_set(GTK_TREE_STORE(model), iter,
		DIVE_NR, dive->number,
		DIVE_LOCATION, location,
		DIVE_CYLINDER, cylinder,
		DIVE_RATING, dive->rating,
		DIVE_SAC, dive->sac,
		DIVE_OTU, dive->otu,
		DIVE_TOTALWEIGHT, total_weight(dive),
		DIVE_SUIT, suit,
		-1);

	free(location);
	free(cylinder);
	free(suit);

	if (model == GTK_TREE_MODEL(dive_list.treemodel))
		othermodel = dive_list.listmodel;
	else
		othermodel = dive_list.treemodel;
	if (othermodel != dive_list.model)
		/* recursive call */
		gtk_tree_model_foreach(GTK_TREE_MODEL(othermodel), set_one_dive, dive);
}

static gboolean set_one_dive(GtkTreeModel *model,
			     GtkTreePath *path,
			     GtkTreeIter *iter,
			     gpointer data)
{
	int idx;
	struct dive *dive;

	/* Get the dive number */
	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	if (idx < 0)
		return FALSE;
	dive = get_dive(idx);
	if (!dive)
		return TRUE;
	if (data && dive != data)
		return FALSE;

	fill_one_dive(dive, model, iter);
	return dive == data;
}

void flush_divelist(struct dive *dive)
{
	GtkTreeModel *model = GTK_TREE_MODEL(dive_list.model);

	gtk_tree_model_foreach(model, set_one_dive, dive);
}

void set_divelist_font(const char *font)
{
	PangoFontDescription *font_desc = pango_font_description_from_string(font);
	gtk_widget_modify_font(dive_list.tree_view, font_desc);
	pango_font_description_free(font_desc);
}

void update_dive_list_units(void)
{
	const char *unit;
	GtkTreeModel *model = GTK_TREE_MODEL(dive_list.model);

	(void) get_depth_units(0, NULL, &unit);
	gtk_tree_view_column_set_title(dive_list.depth, unit);

	(void) get_temp_units(0, &unit);
	gtk_tree_view_column_set_title(dive_list.temperature, unit);

	(void) get_weight_units(0, NULL, &unit);
	gtk_tree_view_column_set_title(dive_list.totalweight, unit);

	gtk_tree_model_foreach(model, set_one_dive, NULL);
}

void update_dive_list_col_visibility(void)
{
	gtk_tree_view_column_set_visible(dive_list.cylinder, visible_cols.cylinder);
	gtk_tree_view_column_set_visible(dive_list.temperature, visible_cols.temperature);
	gtk_tree_view_column_set_visible(dive_list.totalweight, visible_cols.totalweight);
	gtk_tree_view_column_set_visible(dive_list.suit, visible_cols.suit);
	gtk_tree_view_column_set_visible(dive_list.nitrox, visible_cols.nitrox);
	gtk_tree_view_column_set_visible(dive_list.sac, visible_cols.sac);
	gtk_tree_view_column_set_visible(dive_list.otu, visible_cols.otu);
	return;
}

static void fill_dive_list(void)
{
	int i;
	GtkTreeIter iter, parent_iter, *parent_ptr = NULL;
	GtkTreeStore *liststore, *treestore;
	struct dive *last_trip = NULL;
	GList *trip;
	struct dive *dive_trip = NULL;

	/* if we have pre-existing trips, start on the last one */
	trip = g_list_last(dive_trip_list);

	treestore = GTK_TREE_STORE(dive_list.treemodel);
	liststore = GTK_TREE_STORE(dive_list.listmodel);

	i = dive_table.nr;
	while (--i >= 0) {
		struct dive *dive = get_dive(i);

		/* make sure we display the first date of the trip in previous summary */
		if (dive_trip && parent_ptr) {
			gtk_tree_store_set(treestore, parent_ptr,
					DIVE_NR, dive_trip->number,
					DIVE_DATE, dive_trip->when,
					DIVE_LOCATION, dive_trip->location,
					-1);
		}
		/* the dive_trip info might have been killed by a previous UNGROUPED dive */
		if (trip)
			dive_trip = DIVE_TRIP(trip);
		/* tripflag defines how dives are handled;
		 * TF_NONE "not handled yet" - create time based group if autogroup == TRUE
		 * NO_TRIP "set as no group" - simply leave at top level
		 * IN_TRIP "use the trip with the largest trip time (when) that is <= this dive"
		 */
		if (UNGROUPED_DIVE(dive)) {
			/* first dives that go to the top level */
			parent_ptr = NULL;
			dive_trip = NULL;
		} else if (autogroup && !DIVE_IN_TRIP(dive)) {
			if ( ! dive_trip || ! DIVE_FITS_TRIP(dive, dive_trip)) {
				/* allocate new trip - all fields default to 0
				   and get filled in further down */
				dive_trip = alloc_dive();
				dive_trip_list = insert_trip(dive_trip, dive_trip_list);
				trip = FIND_TRIP(dive_trip, dive_trip_list);
			}
		} else { /* either the dive has a trip or we aren't creating trips */
			if (! (trip && DIVE_FITS_TRIP(dive, DIVE_TRIP(trip)))) {
				GList *last_trip = trip;
				trip = PREV_TRIP(trip, dive_trip_list);
				if (! (trip && DIVE_FITS_TRIP(dive, DIVE_TRIP(trip)))) {
					/* we could get here if there are no trips in the XML file
					 * and we aren't creating trips, either.
					 * Otherwise we need to create a new trip */
					if (autogroup) {
						dive_trip = alloc_dive();
						dive_trip_list = insert_trip(dive_trip, dive_trip_list);
						trip = FIND_TRIP(dive_trip, dive_trip_list);
					} else {
						/* let's go back to the last valid trip */
						trip = last_trip;
					}
				} else {
					dive_trip = trip->data;
					dive_trip->number = 0;
				}
			}
		}
		/* update dive_trip to include this dive, increase number of dives in
		   the trip and update location if necessary */
		if (dive_trip) {
			dive->tripflag = IN_TRIP;
			dive_trip->number++;
			dive_trip->when = dive->when;
			if (!dive_trip->location && dive->location)
				dive_trip->location = dive->location;
			if (dive_trip != last_trip) {
				last_trip = dive_trip;
				/* create trip entry */
				gtk_tree_store_append(treestore, &parent_iter, NULL);
				parent_ptr = &parent_iter;
				/* a duration of 0 (and negative index) identifies a group */
				gtk_tree_store_set(treestore, parent_ptr,
						DIVE_INDEX, -1,
						DIVE_NR, dive_trip->number,
						DIVE_DATE, dive_trip->when,
						DIVE_LOCATION, dive_trip->location,
						DIVE_DURATION, 0,
						-1);
			}
		}

		/* store dive */
		update_cylinder_related_info(dive);
		gtk_tree_store_append(treestore, &iter, parent_ptr);
		gtk_tree_store_set(treestore, &iter,
			DIVE_INDEX, i,
			DIVE_NR, dive->number,
			DIVE_DATE, dive->when,
			DIVE_DEPTH, dive->maxdepth,
			DIVE_DURATION, dive->duration.seconds,
			DIVE_LOCATION, dive->location,
			DIVE_RATING, dive->rating,
			DIVE_TEMPERATURE, dive->watertemp.mkelvin,
			DIVE_SAC, 0,
			-1);
		gtk_tree_store_append(liststore, &iter, NULL);
		gtk_tree_store_set(liststore, &iter,
			DIVE_INDEX, i,
			DIVE_NR, dive->number,
			DIVE_DATE, dive->when,
			DIVE_DEPTH, dive->maxdepth,
			DIVE_DURATION, dive->duration.seconds,
			DIVE_LOCATION, dive->location,
			DIVE_RATING, dive->rating,
			DIVE_TEMPERATURE, dive->watertemp.mkelvin,
			DIVE_TOTALWEIGHT, 0,
			DIVE_SUIT, dive->suit,
			DIVE_SAC, 0,
			-1);
	}

	/* make sure we display the first date of the trip in previous summary */
	if (parent_ptr && dive_trip)
		gtk_tree_store_set(treestore, parent_ptr,
				DIVE_NR, dive_trip->number,
				DIVE_DATE, dive_trip->when,
				DIVE_LOCATION, dive_trip->location,
				-1);
	update_dive_list_units();
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dive_list.model), &iter)) {
		GtkTreeSelection *selection;

		/* select the last dive (and make sure it's an actual dive that is selected) */
		gtk_tree_model_get(GTK_TREE_MODEL(dive_list.model), &iter, DIVE_INDEX, &selected_dive, -1);
		first_leaf(GTK_TREE_MODEL(dive_list.model), &iter, &selected_dive);
		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));
		gtk_tree_selection_select_iter(selection, &iter);
	}
}

void dive_list_update_dives(void)
{
	gtk_tree_store_clear(GTK_TREE_STORE(dive_list.treemodel));
	gtk_tree_store_clear(GTK_TREE_STORE(dive_list.listmodel));
	fill_dive_list();
	repaint_dive();
}

static struct divelist_column {
	const char *header;
	data_func_t data;
	sort_func_t sort;
	unsigned int flags;
	int *visible;
} dl_column[] = {
	[DIVE_NR] = { "#", nr_data_func, NULL, ALIGN_RIGHT | UNSORTABLE },
	[DIVE_DATE] = { "Date", date_data_func, NULL, ALIGN_LEFT },
	[DIVE_RATING] = { UTF8_BLACKSTAR, star_data_func, NULL, ALIGN_LEFT },
	[DIVE_DEPTH] = { "ft", depth_data_func, NULL, ALIGN_RIGHT },
	[DIVE_DURATION] = { "min", duration_data_func, NULL, ALIGN_RIGHT },
	[DIVE_TEMPERATURE] = { UTF8_DEGREE "F", temperature_data_func, NULL, ALIGN_RIGHT, &visible_cols.temperature },
	[DIVE_TOTALWEIGHT] = { "lbs", weight_data_func, NULL, ALIGN_RIGHT, &visible_cols.totalweight },
	[DIVE_SUIT] = { "Suit", NULL, NULL, ALIGN_LEFT, &visible_cols.suit },
	[DIVE_CYLINDER] = { "Cyl", NULL, NULL, 0, &visible_cols.cylinder },
	[DIVE_NITROX] = { "O" UTF8_SUBSCRIPT_2 "%", nitrox_data_func, nitrox_sort_func, 0, &visible_cols.nitrox },
	[DIVE_SAC] = { "SAC", sac_data_func, NULL, 0, &visible_cols.sac },
	[DIVE_OTU] = { "OTU", otu_data_func, NULL, 0, &visible_cols.otu },
	[DIVE_LOCATION] = { "Location", NULL, NULL, ALIGN_LEFT },
};


static GtkTreeViewColumn *divelist_column(struct DiveList *dl, struct divelist_column *col)
{
	int index = col - &dl_column[0];
	const char *title = col->header;
	data_func_t data_func = col->data;
	sort_func_t sort_func = col->sort;
	unsigned int flags = col->flags;
	int *visible = col->visible;
	GtkWidget *tree_view = dl->tree_view;
	GtkTreeStore *treemodel = dl->treemodel;
	GtkTreeStore *listmodel = dl->listmodel;
	GtkTreeViewColumn *ret;

	if (visible && !*visible)
		flags |= INVISIBLE;
	ret = tree_view_column(tree_view, index, title, data_func, flags);
	if (sort_func) {
		/* the sort functions are needed in the corresponding models */
		if (index == DIVE_DATE)
			gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(treemodel), index, sort_func, NULL, NULL);
		else
			gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(listmodel), index, sort_func, NULL, NULL);
	}
	return ret;
}

/*
 * This is some crazy crap. The only way to get default focus seems
 * to be to grab focus as the widget is being shown the first time.
 */
static void realize_cb(GtkWidget *tree_view, gpointer userdata)
{
	gtk_widget_grab_focus(tree_view);
}

/*
 * Double-clicking on a group entry will expand a collapsed group
 * and vice versa.
 */
static void collapse_expand(GtkTreeView *tree_view, GtkTreePath *path)
{
	if (!gtk_tree_view_row_expanded(tree_view, path))
		gtk_tree_view_expand_row(tree_view, path, FALSE);
	else
		gtk_tree_view_collapse_row(tree_view, path);

}

/* Double-click on a dive list */
static void row_activated_cb(GtkTreeView *tree_view,
			GtkTreePath *path,
			GtkTreeViewColumn *column,
			gpointer userdata)
{
	int index;
	GtkTreeIter iter;

	if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(dive_list.model), &iter, path))
		return;

	gtk_tree_model_get(GTK_TREE_MODEL(dive_list.model), &iter, DIVE_INDEX, &index, -1);
	/* a negative index is special for the "group by date" entries */
	if (index < 0) {
		collapse_expand(tree_view, path);
		return;
	}
	edit_dive_info(get_dive(index));
}

void add_dive_cb(GtkWidget *menuitem, gpointer data)
{
	struct dive *dive;

	dive = alloc_dive();
	if (add_new_dive(dive)) {
		record_dive(dive);
		report_dives(TRUE);
		return;
	}
	free(dive);
}

void edit_dive_cb(GtkWidget *menuitem, gpointer data)
{
	edit_multi_dive_info(NULL);
}

static void expand_all_cb(GtkWidget *menuitem, GtkTreeView *tree_view)
{
	gtk_tree_view_expand_all(tree_view);
}

static void collapse_all_cb(GtkWidget *menuitem, GtkTreeView *tree_view)
{
	gtk_tree_view_collapse_all(tree_view);
}

static void popup_divelist_menu(GtkTreeView *tree_view, GtkTreeModel *model, int button)
{
	GtkWidget *menu, *menuitem, *image;
	char editlabel[] = "Edit dives";

	menu = gtk_menu_new();
	menuitem = gtk_image_menu_item_new_with_label("Add dive");
	image = gtk_image_new_from_stock(GTK_STOCK_ADD, GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menuitem), image);
	g_signal_connect(menuitem, "activate", G_CALLBACK(add_dive_cb), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
	if (amount_selected) {
		if (amount_selected == 1)
			editlabel[strlen(editlabel) - 1] = '\0';
		menuitem = gtk_menu_item_new_with_label(editlabel);
		g_signal_connect(menuitem, "activate", G_CALLBACK(edit_dive_cb), model);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
	}
	menuitem = gtk_menu_item_new_with_label("Expand all");
	g_signal_connect(menuitem, "activate", G_CALLBACK(expand_all_cb), tree_view);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
	menuitem = gtk_menu_item_new_with_label("Collapse all");
	g_signal_connect(menuitem, "activate", G_CALLBACK(collapse_all_cb), tree_view);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
	gtk_widget_show_all(menu);

	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL,
		button, gtk_get_current_event_time());
}

static void popup_menu_cb(GtkTreeView *tree_view, gpointer userdata)
{
	popup_divelist_menu(tree_view, GTK_TREE_MODEL(dive_list.model), 0);
}

static gboolean button_press_cb(GtkWidget *treeview, GdkEventButton *event, gpointer userdata)
{
	/* Right-click? Bring up the menu */
	if (event->type == GDK_BUTTON_PRESS  &&  event->button == 3) {
		popup_divelist_menu(GTK_TREE_VIEW(treeview), GTK_TREE_MODEL(dive_list.model), 3);
		return TRUE;
	}
	return FALSE;
}

/* we need to have a temporary copy of the selected dives while
   switching model as the selection_cb function keeps getting called
   when gtk_tree_selection_select_path is called.  We also need to
   keep copies of the sort order so we can restore that as well after
   switching models. */
static gboolean second_call = FALSE;
static GtkSortType sortorder[] = { [0 ... DIVELIST_COLUMNS - 1] = GTK_SORT_DESCENDING, };
static int lastcol = DIVE_DATE;

/* Check if this dive was selected previously and select it again in the new model;
 * This is used after we switch models to maintain consistent selections.
 * We always return FALSE to iterate through all dives */
static gboolean set_selected(GtkTreeModel *model, GtkTreePath *path,
				GtkTreeIter *iter, gpointer data)
{
	GtkTreeSelection *selection = GTK_TREE_SELECTION(data);
	int idx, selected;
	struct dive *dive;

	gtk_tree_model_get(model, iter,
		DIVE_INDEX, &idx,
		-1);
	if (idx < 0) {
		GtkTreeIter child;
		if (gtk_tree_model_iter_children(model, &child, iter))
			gtk_tree_model_get(model, &child, DIVE_INDEX, &idx, -1);
	}
	dive = get_dive(idx);
	selected = dive && dive->selected;
	if (selected) {
		gtk_tree_view_expand_to_path(GTK_TREE_VIEW(dive_list.tree_view), path);
		gtk_tree_selection_select_path(selection, path);
	}
	return FALSE;

}

static void update_column_and_order(int colid)
{
	/* Careful: the index into treecolumns is off by one as we don't have a
	   tree_view column for DIVE_INDEX */
	GtkTreeViewColumn **treecolumns = &dive_list.nr;

	/* this will trigger a second call into sort_column_change_cb,
	   so make sure we don't start an infinite recursion... */
	second_call = TRUE;
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(dive_list.model), colid, sortorder[colid]);
	gtk_tree_view_column_set_sort_order(treecolumns[colid - 1], sortorder[colid]);
	second_call = FALSE;
}

/* If the sort column is date (default), show the tree model.
   For every other sort column only show the list model.
   If the model changed, inform the new model of the chosen sort column and make
   sure the same dives are still selected.

   The challenge with this function is that once we change the model
   we also need to change the sort column again (as it was changed in
   the other model) and that causes this function to be called
   recursively - so we need to catch that.
*/
static void sort_column_change_cb(GtkTreeSortable *treeview, gpointer data)
{
	int colid;
	GtkSortType order;
	GtkTreeStore *currentmodel = dive_list.model;

	if (second_call)
		return;

	gtk_tree_sortable_get_sort_column_id(treeview, &colid, &order);
	if(colid == lastcol) {
		/* we just changed sort order */
		sortorder[colid] = order;
		return;
	} else {
		lastcol = colid;
	}
	if(colid == DIVE_DATE)
		dive_list.model = dive_list.treemodel;
	else
		dive_list.model = dive_list.listmodel;
	if (dive_list.model != currentmodel) {
		GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));

		gtk_tree_view_set_model(GTK_TREE_VIEW(dive_list.tree_view), GTK_TREE_MODEL(dive_list.model));
		update_column_and_order(colid);
		gtk_tree_model_foreach(GTK_TREE_MODEL(dive_list.model), set_selected, selection);
	} else {
		if (order != sortorder[colid]) {
			update_column_and_order(colid);
		}
	}
}

GtkWidget *dive_list_create(void)
{
	GtkTreeSelection  *selection;

	dive_list.listmodel = gtk_tree_store_new(DIVELIST_COLUMNS,
				G_TYPE_INT,			/* index */
				G_TYPE_INT,			/* nr */
				G_TYPE_INT,			/* Date */
				G_TYPE_INT,			/* Star rating */
				G_TYPE_INT, 			/* Depth */
				G_TYPE_INT,			/* Duration */
				G_TYPE_INT,			/* Temperature */
				G_TYPE_INT,			/* Total weight */
				G_TYPE_STRING,			/* Suit */
				G_TYPE_STRING,			/* Cylinder */
				G_TYPE_INT,			/* Nitrox */
				G_TYPE_INT,			/* SAC */
				G_TYPE_INT,			/* OTU */
				G_TYPE_STRING			/* Location */
				);
	dive_list.treemodel = gtk_tree_store_new(DIVELIST_COLUMNS,
				G_TYPE_INT,			/* index */
				G_TYPE_INT,			/* nr */
				G_TYPE_INT,			/* Date */
				G_TYPE_INT,			/* Star rating */
				G_TYPE_INT, 			/* Depth */
				G_TYPE_INT,			/* Duration */
				G_TYPE_INT,			/* Temperature */
				G_TYPE_INT,			/* Total weight */
				G_TYPE_STRING,			/* Suit */
				G_TYPE_STRING,			/* Cylinder */
				G_TYPE_INT,			/* Nitrox */
				G_TYPE_INT,			/* SAC */
				G_TYPE_INT,			/* OTU */
				G_TYPE_STRING			/* Location */
				);
	dive_list.model = dive_list.treemodel;
	dive_list.tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(dive_list.model));
	set_divelist_font(divelist_font);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));

	gtk_tree_selection_set_mode(GTK_TREE_SELECTION(selection), GTK_SELECTION_MULTIPLE);
	gtk_widget_set_size_request(dive_list.tree_view, 200, 200);

	dive_list.nr = divelist_column(&dive_list, dl_column + DIVE_NR);
	dive_list.date = divelist_column(&dive_list, dl_column + DIVE_DATE);
	dive_list.stars = divelist_column(&dive_list, dl_column + DIVE_RATING);
	dive_list.depth = divelist_column(&dive_list, dl_column + DIVE_DEPTH);
	dive_list.duration = divelist_column(&dive_list, dl_column + DIVE_DURATION);
	dive_list.temperature = divelist_column(&dive_list, dl_column + DIVE_TEMPERATURE);
	dive_list.totalweight = divelist_column(&dive_list, dl_column + DIVE_TOTALWEIGHT);
	dive_list.suit = divelist_column(&dive_list, dl_column + DIVE_SUIT);
	dive_list.cylinder = divelist_column(&dive_list, dl_column + DIVE_CYLINDER);
	dive_list.nitrox = divelist_column(&dive_list, dl_column + DIVE_NITROX);
	dive_list.sac = divelist_column(&dive_list, dl_column + DIVE_SAC);
	dive_list.otu = divelist_column(&dive_list, dl_column + DIVE_OTU);
	dive_list.location = divelist_column(&dive_list, dl_column + DIVE_LOCATION);

	fill_dive_list();

	g_object_set(G_OBJECT(dive_list.tree_view), "headers-visible", TRUE,
					  "search-column", DIVE_LOCATION,
					  "rules-hint", TRUE,
					  NULL);

	g_signal_connect_after(dive_list.tree_view, "realize", G_CALLBACK(realize_cb), NULL);
	g_signal_connect(dive_list.tree_view, "row-activated", G_CALLBACK(row_activated_cb), NULL);
	g_signal_connect(dive_list.tree_view, "row-expanded", G_CALLBACK(row_expanded_cb), NULL);
	g_signal_connect(dive_list.tree_view, "row-collapsed", G_CALLBACK(row_collapsed_cb), NULL);
	g_signal_connect(dive_list.tree_view, "button-press-event", G_CALLBACK(button_press_cb), NULL);
	g_signal_connect(dive_list.tree_view, "popup-menu", G_CALLBACK(popup_menu_cb), NULL);
	g_signal_connect(selection, "changed", G_CALLBACK(selection_cb), dive_list.model);
	g_signal_connect(dive_list.listmodel, "sort-column-changed", G_CALLBACK(sort_column_change_cb), NULL);
	g_signal_connect(dive_list.treemodel, "sort-column-changed", G_CALLBACK(sort_column_change_cb), NULL);

	gtk_tree_selection_set_select_function(selection, modify_selection_cb, NULL, NULL);

	dive_list.container_widget = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dive_list.container_widget),
			       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(dive_list.container_widget), dive_list.tree_view);

	dive_list.changed = 0;

	return dive_list.container_widget;
}

void mark_divelist_changed(int changed)
{
	dive_list.changed = changed;
}

int unsaved_changes()
{
	return dive_list.changed;
}
