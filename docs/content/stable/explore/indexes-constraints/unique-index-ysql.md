---
title: Unique Indexes
linkTitle:  Unique Indexes
description: Using Unique Indexes in YSQL
image: /images/section_icons/secure/create-roles.png
menu:
  stable:
    identifier: unique-index-ysql
    parent: explore-indexes-constraints
    weight: 210
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="../unique-index-ysql/" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="../unique-index-ycql/" class="nav-link">
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

This example uses the `categories` table from the [Northwind sample database](/latest/sample-data/northwind/). Follow the steps to create a local [cluster](/latest/quick-start/) or in [Yugabyte Cloud](/latest/yugabyte-cloud/cloud-connect/), and [install](/latest/sample-data/northwind/#install-the-northwind-sample-database) the sample Northwind database.

- View the contents of the `categories` table.

```sql
northwind=# SELECT * FROM categories  LIMIT 5;
```

```output
 category_id | category_name  |                        description                         | picture
-------------+----------------+------------------------------------------------------------+---------
           4 | Dairy Products | Cheeses                                                    | \x
           1 | Beverages      | Soft drinks, coffees, teas, beers, and ales                | \x
           2 | Condiments     | Sweet and savory sauces, relishes, spreads, and seasonings | \x
           7 | Produce        | Dried fruit and bean curd                                  | \x
           3 | Confections    | Desserts, candies, and sweet breads                        | \x
(5 rows)
```

- Create a `UNIQUE` index for the `category_id` column in the `categories` table.

```sql
northwind=# CREATE UNIQUE INDEX index_category_id
              ON categories(category_id);
```

- List the index created using the following command:

```sql
northwind=# SELECT indexname,
                   indexdef
            FROM   pg_indexes
            WHERE  tablename = 'categories';
```

```output
     indexname     |                                        indexdef
-------------------+-----------------------------------------------------------------------------------------
 categories_pkey   | CREATE UNIQUE INDEX categories_pkey ON public.categories USING lsm (category_id HASH)
 index_category_id | CREATE UNIQUE INDEX index_category_id ON public.categories USING lsm (category_id HASH)
(2 rows)
```

<!-- Explain does not display it as an Index scan like how it does for others.
northwind=# EXPLAIN SELECT * FROM categories  LIMIT 5;
                              QUERY PLAN
-----------------------------------------------------------------------
 Limit  (cost=0.00..0.50 rows=5 width=114)
   ->  Seq Scan on categories  (cost=0.00..100.00 rows=1000 width=114)
(2 rows) -->

- After the `CREATE` statement is executed, any attempt to insert a new category with an existing `category_id` will result in an error.

```sql
northwind=# INSERT INTO categories(category_id, category_name, description) VALUES (1, 'Savories', 'Spicy chips and snacks');
```

```output
ERROR:  duplicate key value violates unique constraint "categories_pkey"
```

- Insert a row with a new `category_id` and verify its existence in the table.

```sql
northwind=# INSERT INTO categories(category_id, category_name, description) VALUES (9, 'Savories', 'Spicy chips and snacks');
```

```sql
northwind=# SELECT * FROM categories;
```

```output
 category_id | category_name  |                        description                         | picture
-------------+----------------+------------------------------------------------------------+---------
           4 | Dairy Products | Cheeses                                                    | \x
           1 | Beverages      | Soft drinks, coffees, teas, beers, and ales                | \x
           2 | Condiments     | Sweet and savory sauces, relishes, spreads, and seasonings | \x
           7 | Produce        | Dried fruit and bean curd                                  | \x
           9 | Savories       | Spicy chips and snacks                                     |
           3 | Confections    | Desserts, candies, and sweet breads                        | \x
           8 | Seafood        | Seaweed and fish                                           | \x
           5 | Grains/Cereals | Breads, crackers, pasta, and cereal                        | \x
           6 | Meat/Poultry   | Prepared meats                                             | \x
(9 rows)
```

## Learn more

- [Unique index with HASH column ordering](../../../api/ysql/the-sql-language/statements/ddl_create_index/#unique-index-with-hash-column-ordering)
- [Indexes on JSON attributes](../../../explore/json-support/jsonb-ysql/#6-indexes-on-json-attributes)
- [Benefits of Index-only scan](https://blog.yugabyte.com/how-a-distributed-sql-database-boosts-secondary-index-queries-with-index-only-scan/)
