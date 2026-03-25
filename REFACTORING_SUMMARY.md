# Multi-Table Install Filter Enhancement - Refactoring Summary

## Overview
Enhanced the install_filter system to support installing a single routing entry into multiple kernel routing tables simultaneously. Previously, each filter action could only specify a single routing table. Now it can specify multiple tables.

## Key Changes

### 1. Configuration Header (`configuration.h`)
- Added `#define MAX_TABLES_PER_FILTER 32` constant to limit tables per filter
- Modified `struct filter_result`:
  - Replaced: `unsigned int table;`
  - With: `unsigned int tables[MAX_TABLES_PER_FILTER];` and `int table_count;`
- Modified `struct filter`:
  - Changed `action` to directly embed `struct filter_result` (not nested)
  - Simplification: `struct filter_result action;` instead of wrapper struct

### 2. Route Header (`route.h`)
- Added guard definition for `MAX_TABLES_PER_FILTER`
- Modified `struct babel_route`:
  - Replaced: `int installed_table;` (single value, -1 when not installed)
  - With: `int installed_tables[MAX_TABLES_PER_FILTER];` (array of tables)
  - Added: `int installed_table_count;` (number of tables route is in)

### 3. Configuration Parsing (`configuration.c`)
- Updated `parse_filter()` to support multiple 'table' keywords
  - Explicit initialization: `filter->action.table_count = 0;` at function start
  - Multiple table keywords accumulate in `filter->action.tables[]` array
  - `table_count` incremented for each table keyword
  - Bounds checking: Error if more than `MAX_TABLES_PER_FILTER` tables specified
- Updated `update_filter()` function:
  - Copies full table array using `memcpy()`
  - Updates `table_count` field

### 4. Route Management (`route.c`)
- **`change_route()` function** - Major rewrite:
  - Parameters changed: `int *installed_table` → `int *installed_tables, int *installed_table_count`
  - For ROUTE_FLUSH: Uses `route->installed_tables[]` (the tables route is currently in)
    - Ensures cleanup from all tables even if config changed since installation
    - Prevents orphaned routes in kernel tables
  - For ROUTE_ADD/ROUTE_MODIFY: Evaluates current filter result for target tables
  - Loops through each table, calling `kernel_route()` for each
  - Collects successfully installed tables in output array
  - Handles partial failures (continues to other tables if one fails)
  - Returns first error encountered, or success if any table installed

- **`install_route()`**:
  - Passes `route->installed_tables` and `&route->installed_table_count` to track installations

- **`uninstall_route()`**:
  - Calls `change_route(ROUTE_FLUSH, ...)` which uses `route->installed_tables[]`
  - Sets `route->installed_table_count = 0` on success
    - Mimics old behavior where `installed_table` was set to -1

- **`switch_routes()`**:
  - Passes arrays for new route installation
  - Sets `old->installed_table_count = 0` when switching away

- **`change_route_metric()`**:
  - Updated call signature (passes `NULL, NULL` for table tracking)

### 5. Local Monitoring (`local.c`)
- **`local_notify_route_1()`** function:
  - Added `tables_buf[384]` to safely format table array as comma-separated string
    - Buffer size: 32 tables × 10 digits (unsigned) + 31 commas + margin = 384 bytes
    - Safe limit: 370 bytes to prevent overflow
  - Loop builds string representation of all installed tables with format specifier `%u`
  - Output format changed from `table %d` to `tables %s` (comma-separated list)
  - Displays all tables where route is installed

### 6. Cross-route Handling (`xroute.c`)
- Added helper function **`route_installed_in_table()`**:
  - Searches array for specific table value
  - Returns 1 if found, 0 otherwise
- Updated **`flush_duplicate_route()`**:
  - Changed condition from `route->installed_table != kroute->table`
  - To: `!route_installed_in_table(route, kroute->table)`
  - Properly checks if route is in specific table

### 7. Route Dumping (`babeld.c`)
- **`dump_route()`** function:
  - Added `tables_buf[384]` to safely format table array (same size as local.c)
    - Buffer size matches local.c: 32 tables × 10 digits + 31 commas + margin
  - Loop builds comma-separated string of installed tables with format specifier `%u`
  - Output format changed from `table %d` to `tables %s`
  - Displays all tables in debug/status output

## Data Flow

### Configuration Parsing
```
Config: "filter install ip 2001:db8::/32 table 254 table 255"
  → parse_filter() reads "table" keyword
  → accumulates in filter->action.tables[0..N]
  → sets filter->action.table_count
```

### Route Installation
```
insert_route() → install_route()
  → change_route(ROUTE_ADD, route, ..., route->installed_tables, &route->installed_table_count)
    → install_filter() returns filter_result with tables[] array
    → for each table: kernel_route(ADD, table, ...)
    → stores all successfully installed tables in route->installed_tables[]
    → returns route->installed_table_count with total count
```

### Route Flush
```
uninstall_route()
  → change_route(ROUTE_FLUSH, route, ..., NULL, NULL)
    → change_route() detects ROUTE_FLUSH operation
    → uses route->installed_tables[] (not current filter result)
    → loops through all tables where route is currently installed
    → calls kernel_route(FLUSH, table, ...) for each
    → on success: sets route->installed_table_count = 0
  → Ensures cleanup from all tables, regardless of config changes
```

## Backward Compatibility

The changes maintain operational compatibility with single-table configurations:
- Single `table` keyword in filter still works (sets `table_count = 1`)
- Default behavior when no table specified unchanged (uses `export_table`)
- Array with count == 1 behaves like the old single-table case

## Testing Recommendations

1. **Single table configurations**: Verify routes still install correctly with one table
2. **Multi-table configs**: Test filters with multiple table keywords (e.g., `table 254 table 755`)
3. **Table affinity**: Verify routes reach correct kernel tables using `ip route show table N`
4. **Monitoring output**: Check that all tables are listed in debug/status output
5. **Failure modes**: Test behavior when one table installation fails (should continue to others)
6. **Limits**: Test behavior exceeding `MAX_TABLES_PER_FILTER` (should error during parsing)
7. **Config changes**: Test route cleanup when filter config is modified after installation
   - Install route with tables [1, 2, 3]
   - Change config to specify table [4]
   - Uninstall route
   - Verify route is removed from tables 1, 2, 3 (not just 4)

## Potential Future Enhancements

1. Make `MAX_TABLES_PER_FILTER` configurable at compile or runtime
2. Add configuration option for fallback behavior on partial failure
3. Support table ranges or patterns in filter syntax
4. Track per-table installation time/status separately
5. Add per-table statistics in monitoring interface
