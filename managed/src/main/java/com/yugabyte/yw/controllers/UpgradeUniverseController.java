// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.google.inject.Inject;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.common.config.RuntimeConfigFactory;
import com.yugabyte.yw.controllers.handlers.UpgradeUniverseHandler;
import com.yugabyte.yw.forms.CertsRotateParams;
import com.yugabyte.yw.forms.GFlagsUpgradeParams;
import com.yugabyte.yw.forms.PlatformResults.YBPTask;
import com.yugabyte.yw.forms.ResizeNodeParams;
import com.yugabyte.yw.forms.SoftwareUpgradeParams;
import com.yugabyte.yw.forms.SystemdUpgradeParams;
import com.yugabyte.yw.forms.TlsToggleParams;
import com.yugabyte.yw.forms.UpgradeTaskParams;
import com.yugabyte.yw.forms.VMImageUpgradeParams;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import io.swagger.annotations.Api;
import io.swagger.annotations.ApiImplicitParam;
import io.swagger.annotations.ApiImplicitParams;
import io.swagger.annotations.ApiOperation;
import io.swagger.annotations.Authorization;
import java.util.UUID;
import lombok.extern.slf4j.Slf4j;
import play.mvc.Result;

@Slf4j
@Api(
    value = "Universe Upgrades Management",
    authorizations = @Authorization(AbstractPlatformController.API_KEY_AUTH))
public class UpgradeUniverseController extends AuthenticatedController {

  @Inject UpgradeUniverseHandler upgradeUniverseHandler;

  @Inject RuntimeConfigFactory runtimeConfigFactory;

  /**
   * API that restarts all nodes in the universe. Supports rolling and non-rolling restart
   *
   * @param customerUuid ID of customer
   * @param universeUuid ID of universe
   * @return Result of update operation with task id
   */
  @ApiOperation(
      value = "Restart Universe",
      notes = "Queues a task to perform a rolling restart in a universe.",
      nickname = "restartUniverse",
      response = YBPTask.class)
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "upgrade_task_params",
          value = "Upgrade Task Params",
          dataType = "com.yugabyte.yw.forms.UpgradeTaskParams",
          required = true,
          paramType = "body"))
  public Result restartUniverse(UUID customerUuid, UUID universeUuid) {
    return requestHandler(
        upgradeUniverseHandler::restartUniverse,
        UpgradeTaskParams.class,
        customerUuid,
        universeUuid);
  }

  /**
   * API that upgrades YugabyteDB software version in all nodes. Supports rolling and non-rolling
   * upgrade of the universe.
   *
   * @param customerUuid ID of customer
   * @param universeUuid ID of universe
   * @return Result of update operation with task id
   */
  @ApiOperation(
      value = "Upgrade Software",
      notes = "Queues a task to perform software upgrade and rolling restart in a universe.",
      nickname = "upgradeSoftware",
      response = YBPTask.class)
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "software_upgrade_params",
          value = "Software Upgrade Params",
          dataType = "com.yugabyte.yw.forms.SoftwareUpgradeParams",
          required = true,
          paramType = "body"))
  public Result upgradeSoftware(UUID customerUuid, UUID universeUuid) {
    return requestHandler(
        upgradeUniverseHandler::upgradeSoftware,
        SoftwareUpgradeParams.class,
        customerUuid,
        universeUuid);
  }

  /**
   * API that upgrades gflags in all nodes of primary cluster. Supports rolling, non-rolling, and
   * non-restart upgrades upgrade of the universe.
   *
   * @param customerUuid ID of customer
   * @param universeUuid ID of universe
   * @return Result of update operation with task id
   */
  @ApiOperation(
      value = "Upgrade GFlags",
      notes = "Queues a task to perform gflags upgrade and rolling restart in a universe.",
      nickname = "upgradeGFlags",
      response = YBPTask.class)
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "gflags_upgrade_params",
          value = "GFlags Upgrade Params",
          dataType = "com.yugabyte.yw.forms.GFlagsUpgradeParams",
          required = true,
          paramType = "body"))
  public Result upgradeGFlags(UUID customerUuid, UUID universeUuid) {
    return requestHandler(
        upgradeUniverseHandler::upgradeGFlags,
        GFlagsUpgradeParams.class,
        customerUuid,
        universeUuid);
  }

  /**
   * API that rotates custom certificates for onprem universes. Supports rolling and non-rolling
   * upgrade of the universe.
   *
   * @param customerUuid ID of customer
   * @param universeUuid ID of universe
   * @return Result of update operation with task id
   */
  @ApiOperation(
      value = "Upgrade Certs",
      notes = "Queues a task to perform certificate rotation and rolling restart in a universe.",
      nickname = "upgradeCerts",
      response = YBPTask.class)
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "certs_rotate_params",
          value = "Certs Rotate Params",
          dataType = "com.yugabyte.yw.forms.CertsRotateParams",
          required = true,
          paramType = "body"))
  public Result upgradeCerts(UUID customerUuid, UUID universeUuid) {
    return requestHandler(
        upgradeUniverseHandler::rotateCerts, CertsRotateParams.class, customerUuid, universeUuid);
  }

  /**
   * API that toggles TLS state of the universe. Can enable/disable node to node and client to node
   * encryption. Supports rolling and non-rolling upgrade of the universe.
   *
   * @param customerUuid ID of customer
   * @param universeUuid ID of universe
   * @return Result of update operation with task id
   */
  @ApiOperation(
      value = "Upgrade TLS",
      notes = "Queues a task to perform TLS ugprade and rolling restart in a universe.",
      nickname = "upgradeTls",
      response = YBPTask.class)
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "tls_toggle_params",
          value = "TLS Toggle Params",
          dataType = "com.yugabyte.yw.forms.TlsToggleParams",
          required = true,
          paramType = "body"))
  public Result upgradeTls(UUID customerUuid, UUID universeUuid) {
    return requestHandler(
        upgradeUniverseHandler::toggleTls, TlsToggleParams.class, customerUuid, universeUuid);
  }

  /**
   * API that resizes nodes in the universe. Supports only rolling upgrade.
   *
   * @param customerUuid ID of customer
   * @param universeUuid ID of universe
   * @return Result of update operation with task id
   */
  @ApiOperation(
      value = "Resize Node",
      notes = "Queues a task to perform node resize and rolling restart in a universe.",
      nickname = "resizeNode",
      response = YBPTask.class)
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "resize_node_params",
          value = "Resize Node Params",
          dataType = "com.yugabyte.yw.forms.ResizeNodeParams",
          required = true,
          paramType = "body"))
  public Result resizeNode(UUID customerUuid, UUID universeUuid) {
    return requestHandler(
        upgradeUniverseHandler::resizeNode, ResizeNodeParams.class, customerUuid, universeUuid);
  }

  /**
   * API that upgrades VM Image for AWS and GCP based universes. Supports only rolling upgrade of
   * the universe.
   *
   * @param customerUuid ID of customer
   * @param universeUuid ID of universe
   * @return Result of update operation with task id
   */
  @ApiOperation(
      value = "Upgrade VM Image",
      notes = "Queues a task to perform VM Image upgrade and rolling restart in a universe.",
      nickname = "upgradeVMImage",
      response = YBPTask.class)
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "vmimage_upgrade_params",
          value = "VM Image Upgrade Params",
          dataType = "com.yugabyte.yw.forms.VMImageUpgradeParams",
          required = true,
          paramType = "body"))
  public Result upgradeVMImage(UUID customerUuid, UUID universeUuid) {
    Customer customer = Customer.getOrBadRequest(customerUuid);
    Universe universe = Universe.getValidUniverseOrBadRequest(universeUuid, customer);

    if (!runtimeConfigFactory.forUniverse(universe).getBoolean("yb.cloud.enabled")) {
      throw new PlatformServiceException(METHOD_NOT_ALLOWED, "VM image upgrade is disabled.");
    }

    return requestHandler(
        upgradeUniverseHandler::upgradeVMImage,
        VMImageUpgradeParams.class,
        customerUuid,
        universeUuid);
  }

  /**
   * API that upgrades from cron to systemd for universes. Supports only rolling upgrade of the
   * universe.
   *
   * @param customerUUID ID of customer
   * @param universeUUID ID of universe
   * @return Result of update operation with task id
   */
  @ApiOperation(
      value = "Upgrade Systemd",
      notes = "Queues a task to perform systemd upgrade and rolling restart in a universe.",
      nickname = "upgradeSystemd",
      response = YBPTask.class)
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "systemd_upgrade_params",
          value = "Systemd Upgrade Params",
          dataType = "com.yugabyte.yw.forms.SystemdUpgradeParams",
          required = true,
          paramType = "body"))
  public Result upgradeSystemd(UUID customerUUID, UUID universeUUID) {
    return requestHandler(
        upgradeUniverseHandler::upgradeSystemd,
        SystemdUpgradeParams.class,
        customerUUID,
        universeUUID);
  }

  private <T extends UpgradeTaskParams> Result requestHandler(
      IUpgradeUniverseHandlerMethod<T> serviceMethod,
      Class<T> type,
      UUID customerUuid,
      UUID universeUuid) {
    Customer customer = Customer.getOrBadRequest(customerUuid);
    Universe universe = Universe.getValidUniverseOrBadRequest(universeUuid, customer);
    T requestParams =
        UniverseControllerRequestBinder.bindFormDataToUpgradeTaskParams(request(), type);

    log.info(
        "Upgrade for universe {} [ {} ] customer {}.",
        universe.name,
        universe.universeUUID,
        customer.uuid);

    UUID taskUuid = serviceMethod.upgrade(requestParams, customer, universe);
    auditService().createAuditEntryWithReqBody(ctx(), taskUuid);
    return new YBPTask(taskUuid, universe.universeUUID).asResult();
  }
}
