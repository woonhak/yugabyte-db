---
title: Install YugabyteDB on Linux
headerTitle: 1. Install YugabyteDB
linkTitle: 1. Install YugabyteDB
description: Download and install YugabyteDB on Linux (CentOS or Ubuntu) in less than five minutes.
aliases:
  - /quick-start/install/
menu:
  latest:
    parent: quick-start
    name: 1. Install YugabyteDB
    identifier: install-2-linux
    weight: 110
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="../macos/" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="../linux/" class="nav-link active">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="../docker/" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="../kubernetes/" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

## Prerequisites

1. One of the following operating systems:

    * <i class="icon-centos"></i> CentOS 7 or later

    * <i class="icon-ubuntu"></i> Ubuntu 16.04 or later

1. Verify that you have Python 2 or 3 installed.

    ```sh
    $ python --version
    ```

    ```output
    Python 3.7.3
    ```

    {{< note title="Note" >}}

By default, CentOS 8 doesn't have an unversioned system-wide `python` command to avoid locking users to a specific version of Python.
One way to fix this is to set `python3` the alternative for `python` by running: `sudo alternatives --set python /usr/bin/python3`.

Starting from Ubuntu 20.04, `python` isn't available anymore. An easy fix is to install `sudo apt install python-is-python3`.

    {{< /note >}}

1. `wget` or `curl` is available.

    The instructions use the `wget` command to download files. If you prefer to use `curl`, you can replace `wget` with `curl -O`.

    To install `wget`:

    * CentOS: `yum install wget`
    * Ubuntu: `apt install wget`

    To install `curl`:

    * CentOS: `yum install curl`
    * Ubuntu: `apt install curl`

1. Each tablet maps to its own file, so if you experiment with a few hundred tables and a few tablets per table, you can soon end up creating a large number of files in the current shell. Make sure to [configure ulimit values](../../../deploy/manual-deployment/system-config/#ulimits).

## Download YugabyteDB

1. Download the YugabyteDB package using the following `wget` command.

    ```sh
    $ wget https://downloads.yugabyte.com/releases/2.13.0.0/yugabyte-2.13.0.0-b42-linux-x86_64.tar.gz
    ```

    \
    OR:

    ```sh
    $ wget https://downloads.yugabyte.com/releases/2.13.0.0/yugabyte-2.13.0.0-b42-el8-aarch64.tar.gz
    ```

1. Extract the package and then change directories to the YugabyteDB home.

    ```sh
    $ tar xvfz yugabyte-2.13.0.0-b42-linux-x86_64.tar.gz && cd yugabyte-2.13.0.0/
    ```

    \
    OR:

    ```sh
    $ tar xvfz yugabyte-2.13.0.0-b42-el8-aarch64.tar.gz && cd yugabyte-2.13.0.0/
    ```

## Configure YugabyteDB

To configure YugabyteDB, run the following shell script.

```sh
$ ./bin/post_install.sh
```

{{<tip title="Next step" >}}

[Create a local cluster](../../create-local-cluster/linux/)

{{< /tip >}}
