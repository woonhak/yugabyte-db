// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.cronutils.model.Cron;
import com.cronutils.model.definition.CronDefinitionBuilder;
import com.cronutils.model.time.ExecutionTime;
import com.cronutils.parser.CronParser;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.BackupUtil;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.forms.EditBackupScheduleParams;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.PlatformResults;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.forms.PlatformResults.YBPSuccess;
import com.yugabyte.yw.forms.filters.ScheduleApiFilter;
import com.yugabyte.yw.forms.paging.SchedulePagedApiQuery;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Schedule;
import com.yugabyte.yw.models.Schedule.State;
import com.yugabyte.yw.models.ScheduleTask;
import com.yugabyte.yw.models.filters.ScheduleFilter;
import com.yugabyte.yw.models.paging.SchedulePagedQuery;
import com.yugabyte.yw.models.paging.SchedulePagedResponse;
import io.swagger.annotations.Api;
import io.swagger.annotations.ApiImplicitParam;
import io.swagger.annotations.ApiImplicitParams;
import io.swagger.annotations.ApiOperation;
import io.swagger.annotations.Authorization;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import io.swagger.annotations.ApiImplicitParam;
import io.swagger.annotations.ApiImplicitParams;
import java.util.List;
import java.util.UUID;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;
import play.data.Form;
import play.mvc.Result;

import static com.cronutils.model.CronType.UNIX;

@Api(
    value = "Schedule management",
    authorizations = @Authorization(AbstractPlatformController.API_KEY_AUTH))
public class ScheduleController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(ScheduleController.class);

  @ApiOperation(
      value = "List schedules",
      response = Schedule.class,
      responseContainer = "List",
      nickname = "listSchedules")
  public Result list(UUID customerUUID) {
    Customer.getOrBadRequest(customerUUID);

    List<Schedule> schedules = Schedule.getAllActiveByCustomerUUID(customerUUID);
    return PlatformResults.withData(schedules);
  }

  @ApiOperation(
      value = "List schedules V2",
      response = SchedulePagedResponse.class,
      nickname = "listSchedulesV2")
  @ApiImplicitParams(
      @ApiImplicitParam(
          name = "PageScheduleRequest",
          paramType = "body",
          dataType = "com.yugabyte.yw.forms.paging.SchedulePagedApiQuery",
          required = true))
  public Result pageScheduleList(UUID customerUUID) {
    Customer.getOrBadRequest(customerUUID);
    SchedulePagedApiQuery apiQuery = parseJsonAndValidate(SchedulePagedApiQuery.class);
    ScheduleApiFilter apiFilter = apiQuery.getFilter();
    ScheduleFilter filter = apiFilter.toFilter().toBuilder().customerUUID(customerUUID).build();
    SchedulePagedQuery query = apiQuery.copyWithFilter(filter, SchedulePagedQuery.class);

    SchedulePagedResponse schedules = Schedule.pagedList(query);

    return PlatformResults.withData(schedules);
  }

  @ApiOperation(value = "Get Schedule", response = Schedule.class, nickname = "getSchedule")
  public Result get(UUID customerUUID, UUID scheduleUUID) {
    Customer.getOrBadRequest(customerUUID);

    Schedule schedule = Schedule.getOrBadRequest(scheduleUUID);
    return PlatformResults.withData(schedule);
  }

  @ApiOperation(
      value = "Delete a schedule",
      response = PlatformResults.YBPSuccess.class,
      nickname = "deleteSchedule")
  public Result delete(UUID customerUUID, UUID scheduleUUID) {
    Customer.getOrBadRequest(customerUUID);

    Schedule schedule = Schedule.getOrBadRequest(scheduleUUID);

    schedule.stopSchedule();

    auditService().createAuditEntry(ctx(), request());
    return YBPSuccess.empty();
  }

  @ApiOperation(
      value = "Edit a backup schedule V2",
      response = Schedule.class,
      nickname = "editBackupScheduleV2")
  @ApiImplicitParams({
    @ApiImplicitParam(
        required = true,
        dataType = "com.yugabyte.yw.forms.EditBackupScheduleParams",
        paramType = "body")
  })
  public Result editBackupSchedule(UUID customerUUID, UUID scheduleUUID) {
    Customer.getOrBadRequest(customerUUID);
    Schedule schedule = Schedule.getOrBadRequest(customerUUID, scheduleUUID);

    EditBackupScheduleParams params = parseJsonAndValidate(EditBackupScheduleParams.class);
    if (params.status.equals(State.Paused)) {
      throw new PlatformServiceException(
          BAD_REQUEST, "State paused is an internal state and cannot be specified by the user");
    } else if (params.status.equals(State.Stopped)) {
      schedule.stopSchedule();
    } else if (params.status.equals(State.Active)) {
      if (params.frequency == null && params.cronExpression == null) {
        throw new PlatformServiceException(
            BAD_REQUEST, "Both schedule frequency and cron expression cannot be null");
      } else if (params.frequency != null && params.cronExpression != null) {
        throw new PlatformServiceException(
            BAD_REQUEST, "Both schedule frequency and cron expression cannot be provided");
      } else if (schedule.getStatus().equals(State.Active) && schedule.getRunningState()) {
        throw new PlatformServiceException(CONFLICT, "Cannot edit schedule as it is running.");
      } else if (params.frequency != null) {
        BackupUtil.validateBackupFrequency(params.frequency);
        schedule.updateFrequency(params.frequency);
      } else if (params.cronExpression != null) {
        BackupUtil.validateBackupCronExpression(params.cronExpression);
        schedule.updateCronExpression(params.cronExpression);
      }
    }
    auditService().createAuditEntry(ctx(), request());
    return PlatformResults.withData(schedule);
  }

  @ApiOperation(
      value = "Delete a schedule V2",
      response = PlatformResults.YBPSuccess.class,
      nickname = "deleteScheduleV2")
  public Result deleteYb(UUID customerUUID, UUID scheduleUUID) {
    Customer.getOrBadRequest(customerUUID);
    Schedule schedule = Schedule.getOrBadRequest(scheduleUUID);
    if (schedule.getStatus().equals(State.Active) && schedule.getRunningState()) {
      throw new PlatformServiceException(BAD_REQUEST, "Cannot delete schedule as it is running.");
    }
    schedule.stopSchedule();
    ScheduleTask.getAllTasks(scheduleUUID).forEach((scheduleTask) -> scheduleTask.delete());
    schedule.delete();
    auditService().createAuditEntry(ctx(), request());
    return YBPSuccess.empty();
  }
}
