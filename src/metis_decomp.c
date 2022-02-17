#include <exodusII.h>
#include <metis.h>
#include <stdlib.h>

#include "base_mesh.h"
#include "el_elm.h"
#include "el_geom.h"
#include "exo_conn.h"
#include "exo_struct.h"
#include "mm_as.h"
#include "mm_eh.h"
#include "mm_elem_block_structs.h"
#include "mm_fill_fill.h"
#include "mm_mp_const.h"
#include "rd_dpi.h"
#include "rd_exo.h"
#include "rd_mesh.h"
#include "rf_allo.h"
#include "rf_fem.h"
#include "rf_io.h"
#include "std.h"

#define CHECK_EX_ERROR(err, format, ...)                              \
  do {                                                                \
    if (err < 0) {                                                    \
      goma_eh(GOMA_ERROR, __FILE__, __LINE__, format, ##__VA_ARGS__); \
    }                                                                 \
  } while (0)

static void mark_elems_nodes(Exo_DB *monolith,
                             int *partitions,
                             int proc,
                             bool *node_indicator,
                             bool *elem_indicator,
                             int *num_elems,
                             int *num_nodes);

static void put_coordinates(int exoid, Exo_DB *monolith, bool *node_indicator, int num_nodes) {
  dbl *x_coords = malloc(sizeof(dbl) * num_nodes);
  dbl *y_coords = NULL;
  if (monolith->num_dim > 1)
    y_coords = malloc(sizeof(dbl) * num_nodes);
  dbl *z_coords = NULL;
  if (monolith->num_dim > 2)
    z_coords = malloc(sizeof(dbl) * num_nodes);

  int proc_offset = 0;
  for (int i = 0; i < monolith->num_nodes; i++) {
    if (node_indicator[i]) {
      switch (monolith->num_dim) {
      case 3:
        z_coords[proc_offset] = monolith->z_coord[i];
      // fall through
      case 2:
        y_coords[proc_offset] = monolith->y_coord[i];
      // fall through
      case 1:
        x_coords[proc_offset] = monolith->x_coord[i];
        break;
      default:
        GOMA_EH(GOMA_ERROR, "Unknown dimension");
        break;
      }
      proc_offset++;
    }
  }
  GOMA_ASSERT(proc_offset = num_nodes);
  ex_put_coord(exoid, x_coords, y_coords, z_coords);
  ex_put_coord_names(exoid, monolith->coord_names);

  free(x_coords);
  free(y_coords);
  free(z_coords);
}

static void put_conn(int exoid, Exo_DB *monolith, bool *elem_indicator, int *node_global_to_local) {
  int *eb_num_elems = malloc(sizeof(int) * monolith->num_elem_blocks);
  for (int ebn = 0; ebn < monolith->num_elem_blocks; ebn++) {
    eb_num_elems[ebn] = 0;
    for (int elem = monolith->eb_ptr[ebn]; elem < monolith->eb_ptr[ebn + 1]; elem++) {
      if (elem_indicator[elem]) {
        eb_num_elems[ebn] += 1;
      }
    }

    int err = ex_put_block(exoid, EX_ELEM_BLOCK, monolith->eb_id[ebn], monolith->eb_elem_type[ebn],
                           eb_num_elems[ebn], monolith->eb_num_nodes_per_elem[ebn], 0, 0,
                           monolith->eb_num_attr[ebn]);
    CHECK_EX_ERROR(err, "ex_put_blocks elem");

    if (eb_num_elems[ebn] > 0) {
      int *eb_conn_proc =
          malloc(sizeof(int) * (eb_num_elems[ebn] * monolith->eb_num_nodes_per_elem[ebn]));
      int conn_offset = 0;
      for (int elem = 0; elem < monolith->eb_num_elems[ebn]; elem++) {
        int gelem = monolith->eb_ptr[ebn] + elem;
        if (elem_indicator[gelem]) {
          for (int j = 0; j < monolith->eb_num_nodes_per_elem[ebn]; j++) {
            int gnode = monolith->eb_conn[ebn][elem * monolith->eb_num_nodes_per_elem[ebn] + j];
            GOMA_ASSERT(node_global_to_local[gnode] >= 0);
            eb_conn_proc[conn_offset] = node_global_to_local[gnode] + 1;
            conn_offset++;
          }
        }
      }
      err = ex_put_conn(exoid, EX_ELEM_BLOCK, monolith->eb_id[ebn], eb_conn_proc, 0, 0);
      CHECK_EX_ERROR(err, "ex_put_conn elem");
      free(eb_conn_proc);
    }
  }

  free(eb_num_elems);
}

static void
put_node_sets(int exoid, Exo_DB *monolith, bool *node_indicator, int *node_global_to_local) {
  ex_set_specs ns_specs;

  int *ns_num_nodes = calloc(monolith->num_node_sets, sizeof(int));
  int *ns_num_distfacts = calloc(monolith->num_node_sets, sizeof(int));
  int *ns_node_index = calloc(monolith->num_node_sets, sizeof(int));
  int *ns_distfact_index = calloc(monolith->num_node_sets, sizeof(int));

  int total_ns_nodes = 0;
  for (int ns = 0; ns < monolith->num_node_sets; ns++) {
    for (int offset = monolith->ns_node_index[ns];
         offset < monolith->ns_node_index[ns] + monolith->ns_num_nodes[ns]; offset++) {
      int gnode = monolith->ns_node_list[offset];
      if (node_indicator[gnode]) {
        ns_num_nodes[ns] += 1;
        ns_num_distfacts[ns] += 1;
        total_ns_nodes++;
      }
    }
    if (ns > 0) {
      ns_node_index[ns] = ns_num_nodes[ns - 1] + ns_node_index[ns - 1];
      ns_distfact_index[ns] = ns_node_index[ns];
    }
  }

  int *ns_node_list = calloc(total_ns_nodes, sizeof(int));
  dbl *ns_distfact_list = calloc(total_ns_nodes, sizeof(dbl));

  int node_list_offset = 0;
  for (int ns = 0; ns < monolith->num_node_sets; ns++) {
    for (int offset = monolith->ns_node_index[ns];
         offset < monolith->ns_node_index[ns] + monolith->ns_num_nodes[ns]; offset++) {
      int gnode = monolith->ns_node_list[offset];
      if (node_indicator[gnode]) {
        int lnode = node_global_to_local[gnode] + 1;
        GOMA_ASSERT(lnode > 0);
        ns_node_list[node_list_offset] = lnode;
        node_list_offset++;
      }
    }
  }

  GOMA_ASSERT(node_list_offset == total_ns_nodes);

  ns_specs.sets_ids = monolith->ns_id;
  ns_specs.num_entries_per_set = ns_num_nodes;
  ns_specs.num_dist_per_set = ns_num_distfacts;
  ns_specs.sets_entry_index = ns_node_index;
  ns_specs.sets_dist_index = ns_distfact_index;
  ns_specs.sets_entry_list = ns_node_list;
  ns_specs.sets_extra_list = NULL;
  ns_specs.sets_dist_fact = ns_distfact_list;

  int err = ex_put_concat_sets(exoid, EX_NODE_SET, &ns_specs);
  CHECK_EX_ERROR(err, "ex_put_concat_sets node_sets");

  free(ns_num_nodes);
  free(ns_num_distfacts);
  free(ns_node_index);
  free(ns_distfact_index);
  free(ns_node_list);
  free(ns_distfact_list);
}

static void
put_side_sets(int exoid, Exo_DB *monolith, bool *elem_indicator, int *elem_global_to_local) {
  ex_set_specs ss_specs;
  ss_specs.sets_ids = monolith->ss_id;

  int *ss_num_sides = calloc(monolith->num_side_sets, sizeof(int));
  int *ss_num_distfacts = calloc(monolith->num_side_sets, sizeof(int));
  int *ss_elem_index = calloc(monolith->num_side_sets, sizeof(int));
  int *ss_distfact_index = calloc(monolith->num_side_sets, sizeof(int));

  int total_ss_sides = 0;
  int total_distfacts = 0;
  ss_elem_index[0] = 0;
  ss_distfact_index[0] = 0;
  for (int ss = 0; ss < monolith->num_side_sets; ss++) {
    for (int j = monolith->ss_elem_index[ss];
         j < monolith->ss_elem_index[ss] + monolith->ss_num_sides[ss]; j++) {
      int elem = monolith->ss_elem_list[j];
      int side = monolith->ss_side_list[j];

      if (elem_indicator[elem]) {
        ss_num_sides[ss] += 1;
        total_ss_sides++;

        int ebn = find_elemblock_index(elem, monolith);
        int side_nodes[MAX_NODES_PER_SIDE];
        int nodes_per_side;
        get_side_info(monolith->eb_elem_itype[ebn], side, &nodes_per_side, side_nodes);
        total_distfacts += nodes_per_side;
        ss_num_distfacts[ss] += nodes_per_side;
      }
    }
    if (ss > 0) {
      ss_elem_index[ss] = ss_elem_index[ss - 1] + ss_num_sides[ss - 1];

      ss_distfact_index[ss] = total_distfacts;
    }
  }

  int *ss_side_list = calloc(total_ss_sides, sizeof(int));
  int *ss_elem_list = calloc(total_ss_sides, sizeof(int));
  dbl *ss_distfact_list = calloc(total_distfacts, sizeof(dbl));
  int offset = 0;
  for (int ss = 0; ss < monolith->num_side_sets; ss++) {
    for (int j = monolith->ss_elem_index[ss];
         j < monolith->ss_elem_index[ss] + monolith->ss_num_sides[ss]; j++) {
      int elem = monolith->ss_elem_list[j];
      int side = monolith->ss_side_list[j];
      if (elem_indicator[elem]) {
        ss_side_list[offset] = side;
        GOMA_ASSERT(elem_global_to_local[elem] >= 0);
        ss_elem_list[offset] = elem_global_to_local[elem] + 1;
        offset++;
      }
    }
  }

  ss_specs.num_entries_per_set = ss_num_sides;
  ss_specs.num_dist_per_set = ss_num_distfacts;
  ss_specs.sets_entry_index = ss_elem_index;
  ss_specs.sets_dist_index = ss_distfact_index;
  ss_specs.sets_entry_list = ss_elem_list;
  ss_specs.sets_extra_list = ss_side_list;
  ss_specs.sets_dist_fact = ss_distfact_list;
  int err = ex_put_concat_sets(exoid, EX_SIDE_SET, &ss_specs);

  CHECK_EX_ERROR(err, "ex_put_concat_sets side_sets");

  free(ss_num_sides);
  free(ss_elem_index);
  free(ss_distfact_index);
  free(ss_num_distfacts);
  free(ss_side_list);
  free(ss_elem_list);
  free(ss_distfact_list);
}

static void put_loadbal(int exoid,
                        Exo_DB *monolith,
                        bool **node_indicators,
                        int n_parts,
                        int proc,
                        int n_nodes,
                        int num_elems,
                        int *node_global_to_local) {
  int num_boundary_nodes = 0;
  int *shared_nodes = calloc(n_parts, sizeof(int));
  int *proc_nodes = calloc(n_nodes, sizeof(int));
  int proc_nodes_offset = 0;
  for (int i = 0; i < monolith->num_nodes; i++) {
    bool marked = false;
    for (int p = 0; p < n_parts; p++) {
      if (p == proc)
        continue;

      if (node_indicators[p][i] && node_indicators[proc][i]) {
        if (!marked) {
          num_boundary_nodes++;
          marked = true;
        }
        shared_nodes[p] += 1;
      }
    }
    if (!marked && node_indicators[proc][i]) {
      proc_nodes[proc_nodes_offset] = node_global_to_local[i] + 1;
      GOMA_ASSERT(node_global_to_local[i] >= 0);
      proc_nodes_offset++;
    }
  }

  int num_neighbors = 0;
  GOMA_ASSERT(proc_nodes_offset + num_boundary_nodes == n_nodes);
  int **shared_nodes_list = malloc(sizeof(int *) * n_parts);
  int **shared_nodes_proc = malloc(sizeof(int *) * n_parts);
  int *shared_nodes_offset = calloc(n_parts, sizeof(int));

  for (int p = 0; p < n_parts; p++) {
    shared_nodes_list[p] = calloc(shared_nodes[p], sizeof(int));
    shared_nodes_proc[p] = calloc(shared_nodes[p], sizeof(int));
    if (shared_nodes[p] > 0) {
      num_neighbors++;
    }
  }

  int *neighbors = malloc(sizeof(int) * num_neighbors);
  int noff = 0;
  for (int p = 0; p < n_parts; p++) {
    if (shared_nodes[p] > 0) {
      neighbors[noff++] = p;
    }
  }
  int num_internal_nodes = proc_nodes_offset;
  for (int i = 0; i < monolith->num_nodes; i++) {
    bool marked = false;
    for (int p = 0; p < n_parts; p++) {
      if (p == proc)
        continue;

      if (node_indicators[p][i] && node_indicators[proc][i]) {
        if (!marked) {
          marked = true;
        }
        shared_nodes_list[p][shared_nodes_offset[p]] = node_global_to_local[i] + 1;
        shared_nodes_proc[p][shared_nodes_offset[p]] = p;
        shared_nodes_offset[p] += 1;
      }
    }
    if (marked) {
      proc_nodes[proc_nodes_offset] = node_global_to_local[i] + 1;
      GOMA_ASSERT(node_global_to_local[i] >= 0);
      proc_nodes_offset++;
    }
  }

  int ex_error = ex_put_loadbal_param(exoid, num_internal_nodes, num_boundary_nodes, 0, num_elems,
                                      0, num_neighbors, 0, proc);
  CHECK_EX_ERROR(ex_error, "ex_put_loadbal_param");

  int *neighbor_counts = malloc(sizeof(int) * num_neighbors);
  for (int i = 0; i < num_neighbors; i++) {
    neighbor_counts[i] = shared_nodes_offset[neighbors[i]];
  }

  ex_error = ex_put_cmap_params(exoid, neighbors, neighbor_counts, NULL, NULL, proc);
  CHECK_EX_ERROR(ex_error, "ex_put_cmap_params");

  ex_error =
      ex_put_processor_node_maps(exoid, proc_nodes, &(proc_nodes[num_internal_nodes]), NULL, proc);
  CHECK_EX_ERROR(ex_error, "ex_put_processor_node_maps");

  for (int i = 0; i < num_neighbors; i++) {
    ex_error = ex_put_node_cmap(exoid, neighbors[i], shared_nodes_list[neighbors[i]],
                                shared_nodes_proc[neighbors[i]], proc);
    CHECK_EX_ERROR(ex_error, "ex_put_node_cmap cmap %d", i);
  }
  for (int p = 0; p < n_parts; p++) {
    free(shared_nodes_list[p]);
    free(shared_nodes_proc[p]);
  }
  free(shared_nodes_list);
  free(shared_nodes_proc);
  free(shared_nodes_offset);
  free(proc_nodes);
  free(neighbors);
  free(neighbor_counts);
}

goma_error goma_metis_decomposition(char **filenames, int n_files) {

  if (sizeof(idx_t) != sizeof(int)) {
    GOMA_EH(GOMA_ERROR, "Goma expects 32bit Metis, idx_t size = %d", sizeof(idx_t));
  }

  int block_weights[MAX_NUMBER_MATLS] = {0};

  Exo_DB *monolith = alloc_struct_1(Exo_DB, 1);
  Dpi *dpi = alloc_struct_1(Dpi, 1);
  init_exo_struct(monolith);
  init_dpi_struct(dpi);

  goma_error err = rd_exo(monolith, filenames[0], 0,
                          (EXODB_ACTION_RD_INIT + EXODB_ACTION_RD_MESH + EXODB_ACTION_RD_RES0));
  GOMA_EH(err, "rd_exo monolith");

  zero_base(monolith);
  setup_base_mesh(dpi, monolith, 1);
  uni_dpi(dpi, monolith);
  setup_old_exo(monolith, dpi, 1);

  Matilda = alloc_int_1(monolith->num_elem_blocks, 0);
  setup_matilda(monolith, Matilda);

  build_elem_node(monolith);

  build_node_elem(monolith);

  build_elem_elem(monolith);

  build_node_node(monolith);

  // crude block weights
  for (int imtrx = 0; imtrx < upd->Total_Num_Matrices; imtrx++) {
    for (int ebn = 0; ebn < monolith->num_elem_blocks; ebn++) {
      int mn = Matilda[ebn];
      for (int var = V_FIRST; var < V_LAST; var++) {
        if (pd_glob[mn]->e[imtrx][var]) {
          int interp = pd_glob[mn]->i[imtrx][var];
          int ielem_type =
              get_type(monolith->eb_elem_type[ebn], monolith->eb_num_nodes_per_elem[ebn],
                       monolith->eb_num_attr[ebn]);
          int ielem_shape = type2shape(ielem_type);
          int dofs = getdofs(ielem_shape, interp);
          block_weights[ebn] += dofs;
        }
      }
    }
  }

  int *vwgt = malloc(sizeof(int) * monolith->num_elems);

  for (int ebn = 0; ebn < monolith->num_elem_blocks; ebn++) {
    for (int elem = monolith->eb_ptr[ebn]; elem < monolith->eb_ptr[ebn + 1]; elem++) {
      vwgt[elem] = block_weights[ebn];
    }
  }
  int n_con = 1;

  int *elem_adj_pntr = malloc(sizeof(int) * (monolith->num_elems + 1));
  int *elem_adj_list = malloc(sizeof(int) * monolith->elem_elem_pntr[monolith->num_elems]);

  elem_adj_pntr[0] = 0;
  int offset = 0;
  for (int i = 0; i < monolith->num_elems; i++) {
    for (int j = monolith->elem_elem_pntr[i]; j < monolith->elem_elem_pntr[i + 1]; j++) {
      int el = monolith->elem_elem_list[j];
      if (el >= 0) {
        elem_adj_list[offset] = el;
        offset++;
      }
    }
    elem_adj_pntr[i + 1] = offset;
  }

  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);
  options[METIS_OPTION_NUMBERING] = 0;
  options[METIS_OPTION_CONTIG] = 1;

  idx_t edgecut;
  int *partitions = malloc(sizeof(int) * monolith->num_elems);
  idx_t n_parts = Num_Proc;

  if (Decompose_Type == 1 || (Decompose_Type == 0 && n_parts < 8)) {
    DPRINTF(stdout, "\nInternal METIS decomposition using Recursive Bisection.\n\n");
    METIS_PartGraphRecursive(&monolith->num_elems, &n_con, elem_adj_pntr, elem_adj_list, vwgt, NULL,
                             NULL, &n_parts, NULL, NULL, options, &edgecut, partitions);

  } else if (Decompose_Type == 2 || (Decompose_Type == 0 && n_parts >= 8)) {
    DPRINTF(stdout, "\nInternal METIS decomposition using KWAY.\n\n");
    METIS_PartGraphKway(&monolith->num_elems, &n_con, elem_adj_pntr, elem_adj_list, vwgt, NULL,
                        NULL, &n_parts, NULL, NULL, options, &edgecut, partitions);
  }

  bool **node_indicators = malloc(sizeof(bool *) * n_parts);
  bool **elem_indicators = malloc(sizeof(bool *) * n_parts);
  int *num_nodes_part = malloc(sizeof(int) * n_parts);
  int *num_elems_part = malloc(sizeof(int) * n_parts);
  for (int proc = 0; proc < n_parts; proc++) {
    node_indicators[proc] = calloc(monolith->num_nodes, sizeof(bool));
    elem_indicators[proc] = calloc(monolith->num_elems, sizeof(bool));
    mark_elems_nodes(monolith, partitions, proc, node_indicators[proc], elem_indicators[proc],
                     &num_elems_part[proc], &num_nodes_part[proc]);
  }

  for (int file = 0; file < n_files; file++) {
    for (int proc = 0; proc < n_parts; proc++) {
      char proc_name[MAX_FNL];
      strncpy(proc_name, filenames[file], MAX_FNL);

      // now we have partitions we just need to separate the monolith into each piece
      multiname(proc_name, proc, n_parts);

      bool *node_indicator = node_indicators[proc];
      bool *elem_indicator = elem_indicators[proc];

      int num_nodes = num_nodes_part[proc];
      int num_elems = num_elems_part[proc];

      int exoid =
          ex_create(proc_name, EX_CLOBBER, &monolith->comp_wordsize, &monolith->io_wordsize);
      CHECK_EX_ERROR(exoid, "ex_create");
      int err =
          ex_put_init(exoid, monolith->title, monolith->num_dim, num_nodes, num_elems,
                      monolith->num_elem_blocks, monolith->num_node_sets, monolith->num_side_sets);
      CHECK_EX_ERROR(err, "ex_put_init");

      int *global_to_local = malloc(sizeof(int) * monolith->num_nodes);
      int *node_map = malloc(sizeof(int) * num_nodes);
      int *elem_map = malloc(sizeof(int) * num_elems);
      int lnode = 0;
      for (int i = 0; i < monolith->num_nodes; i++) {
        if (node_indicator[i]) {
          global_to_local[i] = lnode;
          node_map[lnode] = dpi->node_index_global[i] + 1;
          lnode++;
        } else {
          global_to_local[i] = -1;
        }
      }
      int *global_to_local_elem = malloc(sizeof(int) * monolith->num_elems);
      int lelem = 0;
      for (int i = 0; i < monolith->num_elems; i++) {
        if (elem_indicator[i]) {
          elem_map[lelem] = dpi->elem_index_global[i] + 1;
          global_to_local_elem[i] = lelem;
          lelem++;
        }
      }

      GOMA_ASSERT(lnode == num_nodes);
      err = ex_put_id_map(exoid, EX_NODE_MAP, node_map);
      CHECK_EX_ERROR(err, "ex_put_id_map EX_NODE_MAP");

      GOMA_ASSERT(lelem == num_elems);
      err = ex_put_id_map(exoid, EX_ELEM_MAP, elem_map);
      CHECK_EX_ERROR(err, "ex_put_id_map EX_ELEM_MAP");

      put_coordinates(exoid, monolith, node_indicator, num_nodes);

      put_conn(exoid, monolith, elem_indicator, global_to_local);

      if (monolith->num_node_sets > 0) {
        put_node_sets(exoid, monolith, node_indicator, global_to_local);
      }

      if (monolith->num_side_sets > 0) {
        put_side_sets(exoid, monolith, elem_indicator, global_to_local_elem);
      }

      err = ex_put_init_global(exoid, monolith->num_nodes, monolith->num_elems,
                               monolith->num_elem_blocks, monolith->num_node_sets,
                               monolith->num_side_sets);
      CHECK_EX_ERROR(err, "ex_put_init_global");

      err = ex_put_ns_param_global(exoid, monolith->ns_id, monolith->ns_num_nodes,
                                   monolith->ns_num_distfacts);
      CHECK_EX_ERROR(err, "ex_put_ns_param_global");

      err = ex_put_ss_param_global(exoid, monolith->ss_id, monolith->ss_num_sides,
                                   monolith->ss_num_distfacts);
      CHECK_EX_ERROR(err, "ex_put_ss_param_global");

      err = ex_put_eb_info_global(exoid, monolith->eb_id, monolith->eb_num_elems);
      CHECK_EX_ERROR(err, "ex_put_eb_info_global");

      err = ex_put_init_info(exoid, n_parts, 1, (char *)"p");
      CHECK_EX_ERROR(err, "ex_put_init_info");

      put_loadbal(exoid, monolith, node_indicators, n_parts, proc, num_nodes, num_elems,
                  global_to_local);

      QA_Record *Q = (QA_Record *)smalloc(sizeof(QA_Record));

      for (int j = 0; j < 4; j++) {
        Q[0][j] = (char *)smalloc(LEN_QA_RECORD * sizeof(char));

        /*
         * Initialize to null terminators...
         */

        for (int k = 0; k < LEN_QA_RECORD; k++) {
          Q[0][j][k] = '\0';
        }
      }
      strcpy(Q[0][0], "GOMA METIS");
      strcpy(Q[0][1], GOMA_VERSION); /* def'd in std.h for now */
      get_date(Q[0][2]);
      get_time(Q[0][3]);

      err = ex_put_qa(exoid, 1, Q);
      CHECK_EX_ERROR(err, "ex_put_qa");

      err = ex_close(exoid);
      CHECK_EX_ERROR(err, "ex_close");

      for (int j = 0; j < 4; j++) {
        free(Q[0][j]);
      }
      free(Q);

      free(global_to_local);
      free(global_to_local_elem);
      free(node_map);
      free(elem_map);
    }
  }

  for (int proc = 0; proc < n_parts; proc++) {
    free(node_indicators[proc]);
    free(elem_indicators[proc]);
  }
  free(num_elems_part);
  free(num_nodes_part);
  free(node_indicators);
  free(elem_indicators);
  free(Matilda);
  free(elem_adj_list);
  free(elem_adj_pntr);
  free(vwgt);
  free_exo(monolith);
  free_dpi_uni(dpi);
  free(monolith);
  free(dpi);
  free(Element_Blocks);

  for (int i = 0; i < MAX_MAT_PER_SS + 1; i++) {
    free(ss_to_blks[i]);
  }
  free(Proc_SS_Node_Count);
  free(Coor);
  return GOMA_SUCCESS;
}

static void mark_elems_nodes(Exo_DB *monolith,
                             int *partitions,
                             int proc,
                             bool *node_indicator,
                             bool *elem_indicator,
                             int *num_elems,
                             int *num_nodes) {
  *num_elems = 0;
  *num_nodes = 0;

  for (int ebn = 0; ebn < monolith->num_elem_blocks; ebn++) {
    for (int elem = monolith->eb_ptr[ebn]; elem < monolith->eb_ptr[ebn + 1]; elem++) {
      if (partitions[elem] == proc) {
        elem_indicator[elem] = true;
        (*num_elems)++;
        for (int offset = monolith->elem_node_pntr[elem];
             offset < monolith->elem_node_pntr[elem + 1]; offset++) {
          int node = monolith->elem_node_list[offset];
          if (!node_indicator[node]) {
            node_indicator[node] = true;
            (*num_nodes)++;
          }
        }
      }
    }
  }
}