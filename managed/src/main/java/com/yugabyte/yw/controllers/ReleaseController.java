// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.yugabyte.yw.cloud.PublicCloudConstants.Architecture;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.common.ReleaseManager;
import com.yugabyte.yw.common.ReleaseManager.ReleaseMetadata;
import com.yugabyte.yw.common.ValidatingFormFactory;
import com.yugabyte.yw.forms.ReleaseFormData;
import com.yugabyte.yw.forms.PlatformResults;
import com.yugabyte.yw.forms.PlatformResults.YBPSuccess;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.helpers.CommonUtils;
import io.swagger.annotations.Api;
import io.swagger.annotations.ApiImplicitParam;
import io.swagger.annotations.ApiImplicitParams;
import io.swagger.annotations.ApiOperation;
import io.swagger.annotations.Authorization;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.stream.Collectors;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;
import play.mvc.Result;

@Api(
    value = "Release management",
    authorizations = @Authorization(AbstractPlatformController.API_KEY_AUTH))
public class ReleaseController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(ReleaseController.class);

  @Inject ReleaseManager releaseManager;

  @Inject ValidatingFormFactory formFactory;

  @ApiOperation(value = "Create a release", response = YBPSuccess.class, nickname = "createRelease")
  @ApiImplicitParams({
    @ApiImplicitParam(
        name = "Release",
        value = "Release data for remote downloading to be created",
        required = true,
        dataType = "com.yugabyte.yw.forms.ReleaseFormData",
        paramType = "body")
  })
  public Result create(UUID customerUUID) {
    Customer customer = Customer.getOrBadRequest(customerUUID);

    Iterator<Map.Entry<String, JsonNode>> it = request().body().asJson().fields();
    List<ReleaseFormData> versionDataList = new ArrayList<>();
    while (it.hasNext()) {
      Map.Entry<String, JsonNode> versionJson = it.next();
      ReleaseFormData formData =
          formFactory.getFormDataOrBadRequest(versionJson.getValue(), ReleaseFormData.class);
      formData.version = versionJson.getKey();
      LOG.info("ReleaseController: Asked to add new release: {} ", formData.version);
      versionDataList.add(formData);
    }

    try {
      Map<String, ReleaseMetadata> releases =
          ReleaseManager.formDataToReleaseMetadata(versionDataList);
      releases.forEach(
          (version, metadata) -> releaseManager.addReleaseWithMetadata(version, metadata));
    } catch (RuntimeException re) {
      throw new PlatformServiceException(INTERNAL_SERVER_ERROR, re.getMessage());
    }

    auditService().createAuditEntry(ctx(), request(), request().body().asJson());
    return YBPSuccess.empty();
  }

  @ApiOperation(
      value = "List all releases",
      response = Object.class,
      responseContainer = "Map",
      nickname = "getListOfReleases")
  public Result list(UUID customerUUID, Boolean includeMetadata) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Map<String, Object> releases = releaseManager.getReleaseMetadata();

    // Filter out any deleted releases
    Map<String, Object> filtered =
        releases
            .entrySet()
            .stream()
            .filter(f -> !Json.toJson(f.getValue()).get("state").asText().equals("DELETED"))
            .collect(Collectors.toMap(p -> p.getKey(), p -> p.getValue()));
    return PlatformResults.withData(
        includeMetadata ? CommonUtils.maskObject(filtered) : filtered.keySet());
  }

  @ApiOperation(
      value = "List all releases valid in region",
      response = Object.class,
      responseContainer = "Map",
      nickname = "getListOfRegionReleases")
  public Result listByRegion(
      UUID customerUUID, UUID providerUUID, UUID regionUUID, Boolean includeMetadata) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Region region = Region.getOrBadRequest(customerUUID, providerUUID, regionUUID);
    Map<String, Object> releases = releaseManager.getReleaseMetadata();
    Architecture arch = region.getArchitecture();
    // Old region without architecture. Return all releases.
    if (arch == null) {
      LOG.info(
          "ReleaseController: Could not determine region {} architecture. Listing all releases.",
          region.code);
      return list(customerUUID, includeMetadata);
    }

    // Filter for active and matching region releases
    Map<String, Object> filtered =
        releases
            .entrySet()
            .stream()
            .filter(f -> !Json.toJson(f.getValue()).get("state").asText().equals("DELETED"))
            .filter(f -> releaseManager.metadataFromObject(f.getValue()).matchesRegion(region))
            .collect(Collectors.toMap(p -> p.getKey(), p -> p.getValue()));
    return PlatformResults.withData(
        includeMetadata ? CommonUtils.maskObject(filtered) : filtered.keySet());
  }

  @ApiOperation(
      value = "Update a release",
      response = ReleaseManager.ReleaseMetadata.class,
      nickname = "updateRelease")
  @ApiImplicitParams({
    @ApiImplicitParam(
        name = "Release",
        value = "Release data to be updated",
        required = true,
        dataType = "Object",
        paramType = "body")
  })
  public Result update(UUID customerUUID, String version) {
    Customer customer = Customer.getOrBadRequest(customerUUID);

    ObjectNode formData;
    ReleaseManager.ReleaseMetadata m = releaseManager.getReleaseByVersion(version);
    if (m == null) {
      throw new PlatformServiceException(BAD_REQUEST, "Invalid Release version: " + version);
    }
    formData = (ObjectNode) request().body().asJson();

    // For now we would only let the user change the state on their releases.
    if (formData.has("state")) {
      String stateValue = formData.get("state").asText();
      LOG.info("Updating release state for version {} to {}", version, stateValue);
      m.state = ReleaseManager.ReleaseState.valueOf(stateValue);
      releaseManager.updateReleaseMetadata(version, m);
    } else {
      throw new PlatformServiceException(BAD_REQUEST, "Missing Required param: State");
    }
    auditService().createAuditEntry(ctx(), request(), Json.toJson(formData));
    return PlatformResults.withData(m);
  }

  @ApiOperation(value = "Refresh a release", response = YBPSuccess.class)
  public Result refresh(UUID customerUUID) {
    Customer customer = Customer.getOrBadRequest(customerUUID);

    LOG.info("ReleaseController: refresh");
    try {
      releaseManager.importLocalReleases();
      releaseManager.updateCurrentReleases();
    } catch (RuntimeException re) {
      throw new PlatformServiceException(INTERNAL_SERVER_ERROR, re.getMessage());
    }
    return YBPSuccess.empty();
  }

  @ApiOperation(
      value = "Delete a release",
      response = ReleaseManager.ReleaseMetadata.class,
      nickname = "deleteRelease")
  public Result delete(UUID customerUUID, String version) {
    if (releaseManager.getReleaseByVersion(version) == null) {
      throw new PlatformServiceException(BAD_REQUEST, "Invalid Release version: " + version);
    }

    if (releaseManager.getInUse(version)) {
      throw new PlatformServiceException(BAD_REQUEST, "Release " + version + " is in use!");
    }
    try {
      releaseManager.removeRelease(version);
    } catch (RuntimeException re) {
      throw new PlatformServiceException(INTERNAL_SERVER_ERROR, re.getMessage());
    }
    return YBPSuccess.empty();
  }
}
