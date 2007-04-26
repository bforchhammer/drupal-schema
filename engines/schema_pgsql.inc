<?php
// $Id$

function schema_pgsql_engine_type_map() {
  static $map;
  if (!isset($map)) {
    // put :normal last so it gets preserved by array_flip
    $map = array(
      'varchar:normal' => 'varchar',
      
      'text:small' => 'text',
      'text:medium' => 'text',
      'text:big' => 'text',
      'text:normal' => 'text',
      
      'int:tiny' => 'smallint',
      'int:small' => 'smallint',
      'int:medium' => 'integer',
      'int:big' => 'bigint',
      'int:normal' => 'integer',
      
      'float:tiny' => 'real',
      'float:small' => 'real',
      'float:medium' => 'real',
      'float:big' => 'double precision',
      'float:normal' => 'real',
      
      'blob:big' => 'bytea',
      'blob:normal' => 'bytea',

      'datetime:normal' => 'timestamp',

      'serial:normal' => 'serial',
      'serial:big' => 'bigserial',
      );
  }
  return $map;
}

function schema_pgsql_schema_type_map() {
  static $map;
  if (!isset($map)) {
    $map = array_flip(schema_pgsql_engine_type_map());
    $map['character varying'] = 'varchar:normal';
  }
  return $map;
}

function schema_pgsql_create_table_sql($table) {
  $sql_cols = array();
  foreach ($table['cols'] as $colname => $col) {
    $sql = $colname.' '.schema_engine_type($col, 'pgsql');
    if ($col['type'] == 'serial') {
      unset($col['not null']);
    }
    unset($col['type']);
    if ($col['length']) {
      $sql .= '('.$col['length'].')';
    }
    unset($col['length']);
    if ($col['not null']) {
      $sql .= ' NOT NULL';
    }
    unset($col['not null']);
    foreach ($col as $prop => $val) {
      switch ($prop) {
        case 'default':
          $sql .= " $prop ";
          if (is_string($val)) {
            $sql .= "'$val'";
          } else {
            $sql .= $val;
          }
          break;
      }
    }
    $sql_cols[] = $sql;
  }

  $sql_keys = array();
  if (is_array($table['primary'])) { // tables indexes are not required
    $sql_keys[] = 'PRIMARY KEY ('.implode(', ', $table['primary']).')';
  }
  if (is_array($table['unique'])) {
    foreach ($table['unique'] as $keyname => $key) {
      $sql_keys[] = 'UNIQUE ('.implode(', ', $key).')';
    }
  }
  $indices = array();
  if (is_array($table['index'])) {
    foreach ($table['index'] as $keyname => $key) {
      $indices[] = ('CREATE INDEX {'.$table['name'].'}_'.
        implode('_', $key).'_idx ON {'.$table['name'].'} ('.
        implode(', ', $key).');');
    }
  }

  $sql = "CREATE TABLE {".$table['name']."} (\n\t";
  $sql .= implode(",\n\t", $sql_cols);
  if (count($sql_keys) > 0) {
    $sql .= ",\n\t";
  }
  $sql .= implode(",\n\t", $sql_keys);
  $sql .= "\n";
  $sql .= ");\n";
  if (count($indices) > 0) {
    $sql .= implode("\n", $indices)."\n";
  }
  $sql .= "\n";
  return $sql;
}

function schema_pgsql_inspect($name = NULL) {
  global $db_url;

  $tables = array();
  $url = parse_url($db_url);
  $database = substr($url['path'], 1);

  $sql = ('SELECT * FROM information_schema.COLUMNS '.
	  'WHERE TABLE_CATALOG=\'%s\' AND TABLE_SCHEMA=\'public\'');
  if (isset($name)) {
    $sql .= 'AND TABLE_NAME = \'%s\' ';
  }
  $sql .= 'ORDER BY TABLE_NAME, ORDINAL_POSITION';
  
  $res = db_query($sql, $database, $name);
  while ($r = db_fetch_array($res)) {
    $numeric = !is_null($r['numeric_scale']);
    $col = array();
    list($col['type'], $col['size']) = schema_schema_type($r['data_type'], 'pgsql');
    if (! $numeric && $r['character_maximum_length']) {
      $col['length'] = $r['character_maximum_length'];
    }
    $col['not null'] = ($r['is_nullable'] == 'YES' ? 0 : 1);
    if (! is_null($r['column_default'])) {
      list($col['default'], $def_type) = explode('::', $r['column_default']);
      if ($numeric) {
        // XXX floats!
        if (strpos($col['default'], 'nextval(\'') !== FALSE &&
          $def_type == 'regclass)') {
          $col['type'] = 'serial';
          unset($col['default']);
        } 
        else {
          $col['default'] = intval($col['default']);
        }
      } else {
        $col['default'] = substr($col['default'], 1, -1);
      }

      // XXX These domains are currently defined in system.install.
      // This engine should probably create and own them, however.
      switch ($r['domain_name']) {
        case 'int_unsigned':
        case 'smallint_unsigned':
        case 'bigint_unsigned':
          $col['unsigned'] = 1;
          break;
      }
    }

    if (FALSE && !$once && $r['table_name'] == 'role') {
      dpm($r);
      dpm($col);
      $once++;
    }

    $tables[$r['table_name']]['cols'][$r['column_name']] = $col;
    $tables[$r['table_name']]['name'] = $r['table_name'];
  }

  $res = db_query('SELECT * FROM information_schema.key_column_usage kcu '.
    'INNER JOIN information_schema.table_constraints tc '.
    '  ON tc.constraint_name=kcu.constraint_name '.
    'WHERE kcu.table_catalog=\'%s\' AND kcu.table_schema=\'public\' '.
    'ORDER BY kcu.table_name, kcu.constraint_name, kcu.ordinal_position',
    $database);
  while ($r = db_fetch_array($res)) {
    $name = $r['constraint_name'];
    if ($r['constraint_type'] == 'PRIMARY KEY') {
      $tables[$r['table_name']]['primary'][] = $r['column_name'];
    } else {
      $tables[$r['table_name']]['unique'][$name][] = $r['column_name'];
    }
  }

  // I truly wish I was kidding with this query, but I'm not.
  // 
  // RoR (unsurprisingly) needs similar code, and I found this
  // monstrous query at http://dev.rubyonrails.org/ticket/6253.  Mine
  // isn't exactly the same as their's, so quite possibly mine is
  // wrong.
  //
  // Also: http://dev.rubyonrails.org/ticket/5819, uses the ANY
  // keyword instead of the OR list but requires PG 8.1.
  $res = db_query(
    'SELECT t.relname as tblname, a.attname, c.relname as idxname, i.indexrelid, i.indkey FROM pg_index i '.
    'INNER JOIN pg_class c ON i.indexrelid = c.oid '.
    'INNER JOIN pg_class t ON i.indrelid = t.oid '.
    'INNER JOIN pg_namespace n ON t.relnamespace = n.oid '.
    'INNER JOIN pg_attribute a ON a.attrelid = t.oid AND ('.
    '   a.attnum = i.indkey[0] OR '.
    '   a.attnum = i.indkey[1] OR '.
    '   a.attnum = i.indkey[2] OR '.
    '   a.attnum = i.indkey[3] OR '.
    '   a.attnum = i.indkey[4] OR '.
    '   a.attnum = i.indkey[5] OR '.
    '   a.attnum = i.indkey[6] OR '.
    '   a.attnum = i.indkey[7] OR '.
    '   a.attnum = i.indkey[8] OR '.
    '   a.attnum = i.indkey[9]) '.
    'WHERE i.indisunique != \'t\' AND i.indisprimary != \'t\' AND '.
    '      n.nspname=\'public\' '.
    'ORDER BY idxname, CASE '.
    '        WHEN i.indkey[0]=a.attnum THEN 0 '.
    '        WHEN i.indkey[1]=a.attnum THEN 1 '.
    '        WHEN i.indkey[2]=a.attnum THEN 2 '.
    '        WHEN i.indkey[3]=a.attnum THEN 3 '.
    '        WHEN i.indkey[4]=a.attnum THEN 4 '.
    '        WHEN i.indkey[5]=a.attnum THEN 5 '.
    '        WHEN i.indkey[6]=a.attnum THEN 6  '.
    '        WHEN i.indkey[7]=a.attnum THEN 7 '.
    '        WHEN i.indkey[8]=a.attnum THEN 8 '.
    '        WHEN i.indkey[9]=a.attnum THEN 9 END ');
  while ($r = db_fetch_array($res)) {
    //dpm($r);
    $tables[$r['tblname']]['index'][$r['idxname']][] = $r['attname'];
  }

  return $tables;
}

?>