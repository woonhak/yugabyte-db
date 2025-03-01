---
title: Unique Indexes
linkTitle: Unique Indexes
description: Using Unique Indexes in YCQL
image: /images/section_icons/secure/create-roles.png
menu:
  latest:
    identifier: unique-index-ycql
    parent: explore-indexes-constraints
    weight: 211
aliases:
   - /latest/explore/ysql-language-features/indexes-1/
   - /latest/explore/indexes-constraints/indexes-1/
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="../unique-index-ysql/" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="../unique-index-ycql/" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

If you need values in some of the columns to be unique, you can specify your index as `UNIQUE`.

When a `UNIQUE` index is applied to two or more columns, the combined values in these columns can't be duplicated in multiple rows. Note that because a `NULL` value is treated as a distinct value, you can have multiple `NULL` values in a column with a `UNIQUE` index.

If a table has a primary key or a `UNIQUE` constraint defined, a corresponding `UNIQUE` index is created automatically.

## Syntax

```ysql
CREATE INDEX index_name ON table_name(column_list);
```

## Example

- Follow the steps to create a cluster [locally](/latest/quick-start/) or in [Yugabyte Cloud](/latest/yugabyte-cloud/cloud-connect/).

- Use the [YCQL shell](/latest/admin/ycqlsh/) for local clusters, or [Connect using Cloud shell](/latest/yugabyte-cloud/cloud-connect/connect-cloud-shell/) for Yugabyte Cloud, to create a keyspace and a table.

```cql
ycqlsh> CREATE KEYSPACE yb_demo;
ycqlsh> USE yb_demo;
ycqlsh> CREATE TABLE employees(employee_no integer,name text,department text, PRIMARY KEY(employee_no));
```

- Create a `UNIQUE` index for the `name` column in the `employees` table to allow only unique names in your table.

```cql
CREATE UNIQUE INDEX index_employee_no ON employees(employee_no);
```

- Use the [DESCRIBE INDEX](/latest/admin/ycqlsh/#describe) command to verify the index creation.

```cql
ycqlsh:yb_demo> DESCRIBE INDEX index_name;
```

```output
CREATE UNIQUE INDEX index_name ON yb_demo.employees (name) INCLUDE (employee_no)
    WITH transactions = {'enabled': 'true'};
```

- Insert values into the table and verify that no duplicate `names` are created.

```cql
ycqlsh:yb_demo> INSERT INTO employees(employee_no, name, department) VALUES (1, 'John', 'Sales');
ycqlsh:yb_demo> INSERT INTO employees(employee_no, name, department) VALUES (2, 'Bob', 'Marketing');
ycqlsh:yb_demo> INSERT INTO employees(employee_no, name, department) VALUES (3, 'Bob', 'Engineering');
```

```output
InvalidRequest: Error from server: code=2200 [Invalid query] message="Execution Error. Duplicate value disallowed by unique index index_name
INSERT INTO employees(employee_no, name, department) VALUES (3, 'Bob', 'Engineering');
       ^^^^
 (ql error -300)"
```

## Learn more

For other examples, refer the [Create a table with a unique index](../../../api/ycql/ddl_create_index/#create-a-table-with-a-unique-index).
