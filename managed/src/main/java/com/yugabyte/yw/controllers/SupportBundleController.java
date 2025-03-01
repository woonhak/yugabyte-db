// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.inject.Inject;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.commissioner.tasks.params.SupportBundleTaskParams;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.common.SupportBundleUtil;
import com.yugabyte.yw.forms.PlatformResults;
import com.yugabyte.yw.forms.PlatformResults.YBPSuccess;
import com.yugabyte.yw.forms.PlatformResults.YBPTask;
import com.yugabyte.yw.forms.SupportBundleFormData;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.SupportBundle;
import com.yugabyte.yw.models.SupportBundle.SupportBundleStatusType;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.TaskType;
import io.swagger.annotations.Api;
import io.swagger.annotations.ApiOperation;
import io.swagger.annotations.Authorization;
import java.io.InputStream;
import java.util.List;
import java.util.UUID;
import lombok.extern.slf4j.Slf4j;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.mvc.Result;

@Api(
    value = "Support Bundle management",
    authorizations = @Authorization(AbstractPlatformController.API_KEY_AUTH))
@Slf4j
public class SupportBundleController extends AuthenticatedController {

  public static final Logger LOG = LoggerFactory.getLogger(SupportBundleController.class);

  @Inject Commissioner commissioner;
  @Inject SupportBundleUtil supportBundleUtil;

  @ApiOperation(
      value = "Create support bundle for specific universe",
      nickname = "createSupportBundle",
      response = YBPTask.class)
  public Result create(UUID customerUUID, UUID universeUUID) {
    JsonNode requestBody = request().body().asJson();
    SupportBundleFormData bundleData =
        formFactory.getFormDataOrBadRequest(requestBody, SupportBundleFormData.class);

    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getValidUniverseOrBadRequest(universeUUID, customer);
    // Do not create support bundle when either backup, update, or universe is paused
    if (universe.getUniverseDetails().backupInProgress
        || universe.getUniverseDetails().updateInProgress
        || universe.getUniverseDetails().universePaused) {
      throw new PlatformServiceException(
          BAD_REQUEST,
          String.format(
              "Cannot create support bundle since the universe %s"
                  + "is currently in a locked/paused state or has backup running",
              universe.universeUUID));
    }

    // Temporarily cannot create for onprem or k8s properly. Will result in empty directories
    CloudType cloudType = universe.getUniverseDetails().getPrimaryCluster().userIntent.providerType;
    if (cloudType == CloudType.onprem || cloudType == CloudType.kubernetes) {
      throw new PlatformServiceException(
          BAD_REQUEST, "Cannot currently create support bundle for onprem or k8s clusters");
    }

    SupportBundle supportBundle = SupportBundle.create(bundleData, universe);
    SupportBundleTaskParams taskParams =
        new SupportBundleTaskParams(supportBundle, bundleData, customer, universe);
    UUID taskUUID = commissioner.submit(TaskType.CreateSupportBundle, taskParams);
    return new YBPTask(taskUUID, supportBundle.getBundleUUID()).asResult();
  }

  @ApiOperation(
      value = "Download support bundle",
      nickname = "downloadSupportBundle",
      response = SupportBundle.class,
      produces = "application/x-compressed")
  public Result download(UUID customerUUID, UUID universeUUID, UUID bundleUUID) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getValidUniverseOrBadRequest(universeUUID, customer);
    SupportBundle bundle = SupportBundle.getOrBadRequest(bundleUUID);

    if (bundle.getStatus() != SupportBundleStatusType.Success) {
      throw new PlatformServiceException(
          NOT_FOUND, String.format("No bundle found for %s", bundleUUID.toString()));
    }
    InputStream is = SupportBundle.getAsInputStream(bundleUUID);
    response()
        .setHeader(
            "Content-Disposition",
            "attachment; filename=" + SupportBundle.get(bundleUUID).getFileName());
    return ok(is).as("application/x-compressed");
  }

  @ApiOperation(
      value = "List all support bundles from a universe",
      response = SupportBundle.class,
      responseContainer = "List",
      nickname = "listSupportBundle")
  public Result list(UUID customerUUID, UUID universeUUID) {
    List<SupportBundle> supportBundles = SupportBundle.getAll(universeUUID);
    return PlatformResults.withData(supportBundles);
  }

  @ApiOperation(
      value = "Get a support bundle from a universe",
      response = SupportBundle.class,
      nickname = "getSupportBundle")
  public Result get(UUID customerUUID, UUID universeUUID, UUID supportBundleUUID) {
    SupportBundle supportBundle = SupportBundle.getOrBadRequest(supportBundleUUID);
    return PlatformResults.withData(supportBundle);
  }

  @ApiOperation(
      value = "Delete a support bundle",
      response = YBPSuccess.class,
      nickname = "deleteSupportBundle")
  public Result delete(UUID customerUUID, UUID universeUUID, UUID bundleUUID) {
    SupportBundle supportBundle = SupportBundle.getOrBadRequest(bundleUUID);

    // Deletes row from the support_bundle db table
    SupportBundle.delete(bundleUUID);

    // Delete the actual archive file
    supportBundleUtil.deleteFile(supportBundle.getPathObject());

    auditService().createAuditEntry(ctx(), request());
    log.info("Successfully deleted the support bundle: " + bundleUUID.toString());
    return YBPSuccess.empty();
  }
}
