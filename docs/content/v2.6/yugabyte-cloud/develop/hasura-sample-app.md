---
title: Running Hasura Applications
headerTitle: Running Hasura Applications
linkTitle: Hasura Applications
description: Run applications in Hasura Cloud and Yugabyte Cloud.
menu:
  v2.6:
    identifier: hasura-application
    parent: yugabyte-cloud
    weight: 710
isTocNested: true
showAsideToc: true
---

This page demonstrates how to deploy an application on Hasura Cloud and Yugabyte Cloud using Hasura's Realtime Poll sample application. This application is built using React and is powered by Hasura GraphQL Engine backed by a Yugabyte Cloud YugabyteDB cluster. It has an interface for users to cast a vote on a poll, and results are updated in an on-screen bar chart in real time.

## Prerequisites

The example has the following prerequisites:

* You have created a cluster on Yugabyte Cloud
* You have created a Hasura project and connected it to your cluster

You will also need the **Admin Secret** of your Hasura project.

For instructions, refer to [Connect Hasura Cloud to Yugabyte Cloud](../hasura-cloud/).

For details on using Hasura Cloud, refer to the [Hasura Cloud documentation](https://hasura.io/docs/latest/graphql/cloud/index.html).

## Install Hasura CLI

You apply database migrations to the Hasura project using Hasura CLI v.2.0 Beta 2. To install Hasura CLI, refer to [Installing the Hasura CLI](https://hasura.io/docs/latest/graphql/core/hasura-cli/install-hasura-cli.html#install-hasura-cli).

Once installed, update to the v.2 Beta.

```sh
$ hasura update-cli --version v2.0.0-beta.2
```

## Download the Realtime Poll application

The Realtime Poll application is available from the Yugabyte GraphQL Apps repo.

```sh
$ git clone https://github.com/yugabyte/yugabyte-graphql-apps
$ cd yugabyte-graphql-apps/realtime-poll
```

## Set up and configure Realtime Poll and apply migrations

You need to configure the Realtime Poll application to use the Hasura Cloud project domain and Admin Secret:

1. On your local computer, navigate to the `hasura` directory in the sample application directory.

    ```sh
    $ cd realtime-poll/hasura
    ```

1. Edit the `config.yaml` file by changing the following parameters:

    * Set `endpoint` to the domain of your Hasura project; for example, `https://yb-realtime-poll.hasura.app`.
    * Set `admin_secret` to the **Admin Secret** you copied from the Hasura Cloud project dashboard.

To migrate the tables and views to the Yugabyte database:

1. Navigate to the `migrations` directory in the `hasura` directory.

    ```sh
    $ cd migrations
    ```

1. Rename the `default` directory to the Database Display Name you assigned to your Yugabyte Cloud database in the Hasura project console; for example, if your Database Display Name is `yb-realtime-polldb`, use the following command:

    ```sh
    $ mv default yb-realtime-polldb
    ```

1. Return to the hasura directory and apply the migration. For example, if your Database Display Name is `yb-realtime-polldb`, use the following command:

    ```sh
    $ cd ..
    $ hasura migrate apply --database-name yb-realtime-polldb
    ```

    This creates the tables and views required for the polling application in the database.

Finally, update the Realtime Poll application with the Hasura Cloud project domain and Admin Secret:

1. Navigate to the `src` directory in the sample application directory.

    ```sh
    $ cd realtime-poll/src
    ```

1. Edit the `apollo.js` file by changing the following parameters:

    * set the `HASURA_GRAPHQL_ENGINE_HOSTNAME` const to the domain of your Hasura project; for example, `yb-realtime-poll.hasura.app`.
    * set the `hasura_secret` variable to the **Admin Secret** you copied from the Hasura Cloud project dashboard.

## Configure the Hasura project

First, expose the tables and relationships to the GraphQL API:

1. Navigate to the **Data** tab in your Hasura Cloud project console. The tables from the Realtime Poll application are listed under **Untracked tables or views**.

1. Click **Track All** and **OK** to confirm to allow the tables to be exposed over the GraphQL API.

    Once the console refreshes, **Untracked foreign-key relationships** lists the relationships.

1. Click **Track All** and **OK** to confirm to allow the relationships to be exposed over the GraphQL API.

Next, add a new Array relationship for the `poll_results` table called `option` as follows:

1. Select the `poll_results` table.

1. Select the **Relationships** tab.

1. Under **Add a new relationship manually**, click **Configure**.

1. Set the following options:

    * Set **Relationship Type** to Array Relationship.
    * In the **Relationship Name** field, enter option.
    * Set **Reference Schema** to public.
    * Set **Reference Table** to option.
    * Set **From** to `poll_id` and **To** to `poll_id`.<br><br>

    ![Add relationships in Hasura Cloud](/images/deploy/yugabyte-cloud/hasura-cloud-relationships-add.png)<br><br>

1. Click **Save**.

## Run Realtime Poll

To run the Realtime Poll application, on your local computer, navigate to the application root (`realtime-poll`) directory and run the following commands:

```sh
$ npm install 
$ npm start
```

Realtime Poll starts on <http://localhost:3000>.

Open a second browser tab, navigate to <http://localhost:3000>, and cast a vote for React. The chart updates automatically using GraphQL Subscriptions.

![Realtime Poll application](/images/deploy/yugabyte-cloud/hasura-realtime-poll.png)

To verify the data being committed to the Yugabyte Cloud instance, run the following subscription query on the **API** tab of the Hasura Cloud project console to retrieve the Poll ID:

```sh
query {
  poll (limit: 10) {
    id
    question
    options (order_by: {id:desc}){
      id
      text
    }
  }
}
```

Note the Poll ID, then run the following query, setting the `pollID` GraphQL Query Variable to the Poll ID you retrieved using the previous query:

```sh
subscription getResult($pollId: uuid!) {
  poll_results (
    order_by: {option_id:desc},
    where: { poll_id: {_eq: $pollId} }
  ) {
    option_id
    option { id text }
    votes
  }
}
```

Set the `$pollID` variable in the Query Variables field.

```sh
{ "$pollId" : "98277113-a7a2-428c-9c8b-0fe7a91bf42c"}
```
