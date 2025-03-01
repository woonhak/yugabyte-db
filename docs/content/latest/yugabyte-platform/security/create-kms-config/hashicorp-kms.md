---
title: Create a KMS configuration using HashiCorp Vault
headerTitle: Create a KMS configuration using HashiCorp Vault
linkTitle: Create a KMS configuration
description: Use Yugabyte Platform to create a KMS configuration for HashiCorp Vault.
aliases:
  - /latest/yugabyte-platform/security/create-kms-config
menu:
  latest:
    parent: security
    identifier: create-kms-config-3-hashicorp-kms
    weight: 27
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="{{< relref "./aws-kms.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      AWS KMS
    </a>
  </li>

  <li >
    <a href="{{< relref "./hashicorp-kms.md" >}}" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      HashiCorp Vault
    </a>
  </li>

</ul>

Encryption at rest uses universe keys to encrypt and decrypt universe data keys. You can use the Yugabyte Platform UI to create key management service (KMS) configurations for generating the required universe keys for one or more YugabyteDB universes. Encryption at rest in Yugabyte Platform supports the use of [HashiCorp Vault](https://www.vaultproject.io/) as a KMS.

## Configure HashiCorp Vault

Before you can start configuring HashiCorp Vault, install it on a virtual machine, as per instructions provided in [Install Vault](https://www.vaultproject.io/docs/install). The vault can be set up as a multi-node cluster. Ensure that your vault installation meets the following requirements: 

- Has transit secret engine enabled.
- Its seal and unseal mechanism is secure and repeatable.
- Its token creation mechanism is repeatable.

You need to configure HashiCorp Vault in order to use it with Yugabyte Platform, as follows: 

- Create a vault configuration file that references your nodes and specifies the address, as follows:

  ```properties
  storage "raft" {
   path  = "./vault/data/"
   node_id = "node1"
  }
  
  listener "tcp" {
   address   = "127.0.0.1:8200"
   tls_disable = "true"
  }
  
  api_addr = "http://127.0.0.1:8200"
  cluster_addr = "https://127.0.0.1:8201"
  ui = true
  disable_mlock = true
  default_lease_ttl = "768h"
  max_lease_ttl = "8760h"
  ```

  <br>Replace `127.0.0.1` with the vault web address.

  For additional configuration options, see [Parameters](https://www.vaultproject.io/docs/configuration#parameters).

- Initialize the vault server by following instructions provided in [operator init](https://www.vaultproject.io/docs/commands/operator/init).

- Allow access to the vault by following instructions provided in [Unsealing](https://www.vaultproject.io/docs/concepts/seal#unsealing).

- Enable the secret engine by executing the following command:

  ```shell
  vault secrets enable transit 
  ```

  <br>For more information, see [Transit Secrets Engine](https://www.vaultproject.io/docs/secrets/transit) and [Setup](https://www.vaultproject.io/docs/secrets/transit#setup).

- Create the vault policy, as per the following sample:

  ```properties
  path "transit/*" {
    capabilities = ["create", "read", "update", "delete", "list"]
  }
  
  path "auth/token/lookup-self" {
          capabilities = ["read"]
  }
  
  path "sys/capabilities-self" {
          capabilities = ["read", "update"]
  }
  
  path "auth/token/renew-self" {
          capabilities = ["update"]
  }
  
  path "sys/*" {
          capabilities = ["read"]
  }
  ```

- Generate a token with appropriate permissions (as per the referenced policy) by executing the following command:

  ```shell
  vault token create -no-default-policy -policy=trx
  ```

  <br>You may also specify the following for your token: 

  - `ttl` — Time to live (TTL). If not specified, the default TTL of 32 days is used, which means that the generated token will expire after 32 days.

  - `period` — If specified, the token can be infinitely renewed.

## Create a KMS configuration

You can create a new KMS configuration that uses HashiCorp Vault as follows:

1. Using the Yugabyte Platform UI, navigate to **Configs > Security > Encryption At Rest** to open a list of existing configurations.

2. Click **Create New Config**.

3. Provide the following configuration details:

    - **Configuration Name** — Enter a meaningful name for your configuration.
    - **KMS Provider** — Select **Hashicorp Vault**.
    - **Vault Address** — Enter the web address of your vault. For example, `http://127.0.0.1:8200`
    - **Secret Token** — Enter the token you obtained from the vault.
    - **Secret Engine** — This is a read-only field with its value set to `transit`. It identifies the secret engine.
    - **Mount Path** — Specify the path to the secret engine within the vault. The default value is `transit/`.<br><br>
    
    ![Create config](/images/yp/security/hashicorp-config.png)<br>
    
4. Click **Save**. Your new configuration should appear in the list of configurations.

6. Optionally, to confirm that the information is correct, click **Show details**. Note that sensitive configuration values are displayed partially masked.

## Modify a KMS configuration

You can modify an existing KMS configuration as follows:

1. Using the Yugabyte Platform UI, navigate to **Configs > Security > Encryption At Rest** to open a list of existing configurations.

2. Find the configuration you want to modify and click its corresponding **Actions > Edit Configuration**.

3. Provide new values for the **Vault Address** and **Secret Token** fields.

4. Click **Save**.

5. Optionally, to confirm that the information is correct, click **Show details** or **Actions > Details**.

## Delete a KMS configuration

To delete a KMS configuration, click its corresponding **Actions > Delete Configuration**.

Note that a saved KMS configuration can only be deleted if it is not in use by any existing universes.