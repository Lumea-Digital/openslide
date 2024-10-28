/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

/*
 * Optra (tif, otif) support
 *
 * quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */


#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#define MIN_THUMBNAIL_DIM   500
static const char XML_ROOT_TAG[] = "ScanInfo";

struct optra_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

static void destroy(openslide_t *osr) {
  struct optra_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct optra_ops_data, data);

  for (int32_t i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    _openslide_grid_destroy(l->grid);
    g_slice_free(struct level, l);
  }
  g_free(osr->levels);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *arg,
                      GError **err) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    if (!_openslide_tiff_read_tile(tiffl, tiff,
                                   tiledata, tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, tiledata,
                                   tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
                         tiledata, tw * th * 4,
                         &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tw, th,
                                                                 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct optra_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  struct _openslide_cached_tiff tiff = _openslide_tiffcache_get(data->tc, err);
  if (tiff.tiff == NULL) {
    return false;
  }

  bool success = _openslide_grid_paint_region(l->grid, cr, tiff.tiff,
                                              x / l->base.downsample,
                                              y / l->base.downsample,
                                              level, w, h,
                                              err);
  _openslide_cached_tiff_put(&tiff);

  return success;
}

static const struct _openslide_ops optra_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static xmlNode* get_initial_root_xml(xmlDoc* doc, GError** err) {
    xmlNode* root = xmlDocGetRootElement(doc);
    if (!xmlStrcmp(root->name, BAD_CAST XML_ROOT_TAG)) {
        // /ScanInfo
        return root;

    }
    else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
            "Unrecognized root element in optrascan XML");
        return false;
    }
}

static bool parse_initial_xml(openslide_t* osr, const char* xml,
    GError** err) {
    // parse
    xmlDoc* doc = _openslide_xml_parse(xml, err);
    if (!doc) {
        return false;
    }

    // get scaninfo element
    xmlNode* scaninfo = get_initial_root_xml(doc, err);
    if (!scaninfo) {
        goto FAIL;
    }

    // copy all scaninfo attributes to vendor properties
    for (xmlAttr* attr = scaninfo->properties; attr; attr = attr->next) {
        xmlChar* value = xmlGetNoNsProp(scaninfo, attr->name);
        if (value && *value) {
            g_hash_table_insert(osr->properties,
                g_strdup_printf("optra.%s", attr->name),
                g_strdup((char*)value));
        }
        xmlFree(value);
    }

    // set standard properties
    _openslide_duplicate_int_prop(osr, "optra.Magnification",
        OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
    _openslide_duplicate_double_prop(osr, "optra.PixelResolution",
        OPENSLIDE_PROPERTY_NAME_MPP_X);
    _openslide_duplicate_double_prop(osr, "optra.PixelResolution",
        OPENSLIDE_PROPERTY_NAME_MPP_Y);

    // clean up
    xmlFreeDoc(doc);
    return true;

FAIL:
    xmlFreeDoc(doc);
    return false;
}

static bool optra_detect(const char *filename G_GNUC_UNUSED,
                                struct _openslide_tifflike *tl,
                                GError **err) {
  // ensure we have a TIFF
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // ensure TIFF is tiled
  if (!_openslide_tifflike_is_tiled(tl, 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFF is not tiled");
    return false;
  }
  //check xml packet
  const char* xml = _openslide_tifflike_get_buffer(tl, 0, TIFFTAG_XMLPACKET,
      err);
  if (!xml) {
      return false;
  }

  // check for plausible XML string before parsing
  if (!strstr(xml, XML_ROOT_TAG)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
          "%s not in XMLPacket", XML_ROOT_TAG);
      return false;
  }

  // parse
  xmlDoc* doc = _openslide_xml_parse(xml, err);
  if (!doc) {
      return false;
  }

  // check for ScanInfo element in the xml after parse.
  if (!get_initial_root_xml(doc, err)) {
      xmlFreeDoc(doc);
      return false;
  }

  xmlFreeDoc(doc);
  return true;
}

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct level *la = *(const struct level **) a;
  const struct level *lb = *(const struct level **) b;

  if (la->tiffl.image_w > lb->tiffl.image_w) {
    return -1;
  } else if (la->tiffl.image_w == lb->tiffl.image_w) {
    return 0;
  } else {
    return 1;
  }
}

static bool optra_open(openslide_t *osr,
                              const char *filename,
                              struct _openslide_tifflike *tl,
                              struct _openslide_hash *quickhash1,
                              GError **err) {
  GPtrArray *level_array = g_ptr_array_new();

  // open TIFF
  struct _openslide_tiffcache *tc = _openslide_tiffcache_create(filename);
  struct _openslide_cached_tiff cached_tiff = _openslide_tiffcache_get(tc, err);
  TIFF* tiff = cached_tiff.tiff;
  if (!tiff) {
    goto FAIL;
  }

  // parse initial XML
  const char* xml = _openslide_tifflike_get_buffer(tl, 0, TIFFTAG_XMLPACKET,
      err);
  if (!xml) {
      goto FAIL;
  }
  if (!parse_initial_xml(osr, xml, err)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
          "optra parse initial xml failed");
      goto FAIL;
  }

  tdir_t tn_dir = TIFFCurrentDirectory(tiff);
  // accumulate tiled levels
  do {
    // confirm that this directory is tiled
    if (!TIFFIsTiled(tiff)) {
      continue;
    }

    // confirm subfiletype is available
    if (TIFFCurrentDirectory(tiff) != 0) {
      uint32_t subfiletype;
      if (!TIFFGetField(tiff, TIFFTAG_SUBFILETYPE, &subfiletype)) {
        continue;
      }

      //if not reducedimage, it is meta-data
      if (!(subfiletype & FILETYPE_REDUCEDIMAGE)) {
          //read image description and add as associated image.
          char* image_desc;
          if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
              g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "reading image description failed.");
              goto FAIL;
          }
          if (!_openslide_tiff_add_associated_image(osr, image_desc, tc, TIFFCurrentDirectory(tiff), NULL, err)) {
              goto FAIL;
          }
        continue;
      }
      else
      {
          uint32_t imwidth = 0, imheight = 0;
          if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &imwidth)) {
              g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "reading image width failed");
              goto FAIL;
          }
          if (!TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &imheight)) {
              g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "reading image height failed");
              goto FAIL;
          }
          if ((imwidth > MIN_THUMBNAIL_DIM) && (imheight > MIN_THUMBNAIL_DIM))
              tn_dir = TIFFCurrentDirectory(tiff);//this will be over-written until last level
      }
    }

    // verify that we can read this compression (hard fail if not)
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read compression scheme");
      goto FAIL;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }

    // create level
    struct level *l = g_slice_new0(struct level);
    struct _openslide_tiff_level *tiffl = &l->tiffl;
    if (!_openslide_tiff_level_init(tiff,
                                    TIFFCurrentDirectory(tiff),
                                    (struct _openslide_level *) l,
                                    tiffl,
                                    err)) {
      g_slice_free(struct level, l);
      goto FAIL;
    }
    l->grid = _openslide_grid_create_simple(osr,
                                            tiffl->tiles_across,
                                            tiffl->tiles_down,
                                            tiffl->tile_w,
                                            tiffl->tile_h,
                                            read_tile);

    // add to array
    g_ptr_array_add(level_array, l);
  } while (TIFFReadDirectory(tiff));

  //add last reduced page as thumbnail image.
  //go to last level page
  if (!_openslide_tiff_set_dir(tiff, tn_dir, err)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
          "adding last level as thumbnail failed");
      goto FAIL;
  }
  //add last level page as associated thumbnail image.
  if (!_openslide_tiff_add_associated_image(osr, "thumbnail", tc, TIFFCurrentDirectory(tiff), NULL, err)) {
      goto FAIL;
  }

  // sort tiled levels
  g_ptr_array_sort(level_array, width_compare);

  // set hash and properties
  struct level *top_level = level_array->pdata[level_array->len - 1];
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    top_level->tiffl.dir,
                                                    0,
                                                    err)) {
    goto FAIL;
  }

  // unwrap level array
  int32_t level_count = level_array->len;
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct optra_ops_data *data =
    g_slice_new0(struct optra_ops_data);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &optra_ops;

  // put TIFF handle and store tiffcache reference
  _openslide_cached_tiff_put(&cached_tiff);
  data->tc = tc;

  return true;

 FAIL:
  // free the level array
  if (level_array) {
    for (uint32_t n = 0; n < level_array->len; n++) {
      struct level *l = level_array->pdata[n];
      _openslide_grid_destroy(l->grid);
      g_slice_free(struct level, l);
    }
    g_ptr_array_free(level_array, true);
  }
  // free TIFF
  _openslide_cached_tiff_put(&cached_tiff);
  _openslide_tiffcache_destroy(tc);
  return false;
}

const struct _openslide_format _openslide_format_optra = {
  .name = "optra",
  .vendor = "optra",
  .detect = optra_detect,
  .open = optra_open,
};
