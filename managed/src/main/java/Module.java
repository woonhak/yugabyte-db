// Copyright (c) YugaByte, Inc.

import com.google.inject.AbstractModule;
import com.google.inject.Provides;
import com.yugabyte.yw.cloud.AWSInitializer;
import com.yugabyte.yw.cloud.aws.AWSCloudModule;
import com.yugabyte.yw.commissioner.BackupGarbageCollector;
import com.yugabyte.yw.commissioner.CallHome;
import com.yugabyte.yw.common.GFlagsValidation;
import com.yugabyte.yw.common.metrics.PlatformMetricsProcessor;
import com.yugabyte.yw.commissioner.DefaultExecutorServiceProvider;
import com.yugabyte.yw.commissioner.ExecutorServiceProvider;
import com.yugabyte.yw.commissioner.HealthChecker;
import com.yugabyte.yw.commissioner.SetUniverseKey;
import com.yugabyte.yw.commissioner.SupportBundleCleanup;
import com.yugabyte.yw.commissioner.TaskExecutor;
import com.yugabyte.yw.commissioner.TaskGarbageCollector;
import com.yugabyte.yw.common.AccessManager;
import com.yugabyte.yw.common.AlertManager;
import com.yugabyte.yw.common.ConfigHelper;
import com.yugabyte.yw.common.CustomerTaskManager;
import com.yugabyte.yw.common.ExtraMigrationManager;
import com.yugabyte.yw.common.HealthManager;
import com.yugabyte.yw.common.KubernetesManager;
import com.yugabyte.yw.common.ShellKubernetesManager;
import com.yugabyte.yw.common.NativeKubernetesManager;
import com.yugabyte.yw.common.NetworkManager;
import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.common.PlatformInstanceClientFactory;
import com.yugabyte.yw.common.ReleaseManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.SwamperHelper;
import com.yugabyte.yw.common.TemplateManager;
import com.yugabyte.yw.common.YamlWrapper;
import com.yugabyte.yw.common.YcqlQueryExecutor;
import com.yugabyte.yw.common.YsqlQueryExecutor;
import com.yugabyte.yw.common.alerts.AlertConfigurationWriter;
import com.yugabyte.yw.common.alerts.AlertsGarbageCollector;
import com.yugabyte.yw.common.alerts.QueryAlerts;
import com.yugabyte.yw.common.config.RuntimeConfigFactory;
import com.yugabyte.yw.common.config.impl.SettableRuntimeConfigFactory;
import com.yugabyte.yw.common.ha.PlatformReplicationHelper;
import com.yugabyte.yw.common.ha.PlatformReplicationManager;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.kms.util.EncryptionAtRestUniverseKeyCache;
import com.yugabyte.yw.common.metrics.PlatformMetricsProcessor;
import com.yugabyte.yw.common.services.LocalYBClientService;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.common.ybflyway.YBFlywayInit;
import com.yugabyte.yw.common.SupportBundleUtil;
import com.yugabyte.yw.controllers.MetricGrafanaController;
import com.yugabyte.yw.controllers.PlatformHttpActionAdapter;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import com.yugabyte.yw.queries.QueryHelper;
import com.yugabyte.yw.scheduler.Scheduler;
import lombok.extern.slf4j.Slf4j;
import org.pac4j.core.client.Clients;
import org.pac4j.core.config.Config;
import org.pac4j.oidc.client.OidcClient;
import org.pac4j.oidc.config.OidcConfiguration;
import org.pac4j.oidc.profile.OidcProfile;
import org.pac4j.play.CallbackController;
import org.pac4j.play.store.PlayCacheSessionStore;
import org.pac4j.play.store.PlaySessionStore;
import play.Configuration;
import play.Environment;

import javax.persistence.PersistenceException;

/**
 * This class is a Guice module that tells Guice to bind different types
 *
 * <p>Play will automatically use any class called 'Module' in the root package
 */
@Slf4j
public class Module extends AbstractModule {

  private final Environment environment;
  private final Configuration config;

  public Module(Environment environment, Configuration config) {
    this.environment = environment;
    this.config = config;
  }

  @Override
  public void configure() {
    if (!config.getBoolean("play.evolutions.enabled")) {
      // We want to init flyway only when evolutions are not enabled
      bind(YBFlywayInit.class).asEagerSingleton();
    } else {
      log.info("Using Evolutions. Not using flyway migrations.");
    }

    bind(RuntimeConfigFactory.class).to(SettableRuntimeConfigFactory.class).asEagerSingleton();
    // TODO: other clouds
    install(new AWSCloudModule());

    // Bind Application Initializer
    bind(AppInit.class).asEagerSingleton();
    bind(ConfigHelper.class).asEagerSingleton();
    // Set LocalClientService as the implementation for YBClientService
    bind(YBClientService.class).to(LocalYBClientService.class);
    bind(YsqlQueryExecutor.class).asEagerSingleton();
    bind(YcqlQueryExecutor.class).asEagerSingleton();
    bind(PlaySessionStore.class).to(PlayCacheSessionStore.class);

    // We only needed to bind below ones for Platform mode.
    if (config.getString("yb.mode", "PLATFORM").equals("PLATFORM")) {
      bind(SwamperHelper.class).asEagerSingleton();
      bind(HealthManager.class).asEagerSingleton();
      bind(NodeManager.class).asEagerSingleton();
      bind(MetricQueryHelper.class).asEagerSingleton();
      bind(QueryHelper.class).asEagerSingleton();
      bind(ShellProcessHandler.class).asEagerSingleton();
      bind(NetworkManager.class).asEagerSingleton();
      bind(AccessManager.class).asEagerSingleton();
      bind(ReleaseManager.class).asEagerSingleton();
      bind(TemplateManager.class).asEagerSingleton();
      bind(ExtraMigrationManager.class).asEagerSingleton();
      bind(AWSInitializer.class).asEagerSingleton();
      bind(CallHome.class).asEagerSingleton();
      bind(Scheduler.class).asEagerSingleton();
      bind(HealthChecker.class).asEagerSingleton();
      bind(TaskGarbageCollector.class).asEagerSingleton();
      bind(BackupGarbageCollector.class).asEagerSingleton();
      bind(SupportBundleCleanup.class).asEagerSingleton();
      bind(EncryptionAtRestManager.class).asEagerSingleton();
      bind(EncryptionAtRestUniverseKeyCache.class).asEagerSingleton();
      bind(SetUniverseKey.class).asEagerSingleton();
      bind(CustomerTaskManager.class).asEagerSingleton();
      bind(YamlWrapper.class).asEagerSingleton();
      bind(AlertManager.class).asEagerSingleton();
      bind(QueryAlerts.class).asEagerSingleton();
      bind(PlatformMetricsProcessor.class).asEagerSingleton();
      bind(AlertsGarbageCollector.class).asEagerSingleton();
      bind(AlertConfigurationWriter.class).asEagerSingleton();
      bind(PlatformReplicationManager.class).asEagerSingleton();
      bind(PlatformInstanceClientFactory.class).asEagerSingleton();
      bind(PlatformReplicationHelper.class).asEagerSingleton();
      bind(GFlagsValidation.class).asEagerSingleton();
      bind(ExecutorServiceProvider.class).to(DefaultExecutorServiceProvider.class);
      bind(TaskExecutor.class).asEagerSingleton();
      bind(ShellKubernetesManager.class).asEagerSingleton();
      bind(NativeKubernetesManager.class).asEagerSingleton();
      bind(SupportBundleUtil.class).asEagerSingleton();
      bind(MetricGrafanaController.class).asEagerSingleton();

      final CallbackController callbackController = new CallbackController();
      callbackController.setDefaultUrl(config.getString("yb.url", ""));
      bind(CallbackController.class).toInstance(callbackController);
    }
  }

  @Provides
  protected OidcClient<OidcProfile, OidcConfiguration> provideOidcClient(
      RuntimeConfigFactory runtimeConfigFactory) {
    final OidcConfiguration oidcConfiguration = new OidcConfiguration();

    try {
      if (runtimeConfigFactory.globalRuntimeConf().getString("yb.security.type").equals("OIDC")) {
        oidcConfiguration.setClientId(
            runtimeConfigFactory.globalRuntimeConf().getString("yb.security.clientID"));
        oidcConfiguration.setSecret(
            runtimeConfigFactory.globalRuntimeConf().getString("yb.security.secret"));
        oidcConfiguration.setScope(
            runtimeConfigFactory.globalRuntimeConf().getString("yb.security.oidcScope"));
        oidcConfiguration.setDiscoveryURI(
            runtimeConfigFactory.globalRuntimeConf().getString("yb.security.discoveryURI"));
        oidcConfiguration.setMaxClockSkew(3600);
        oidcConfiguration.setResponseType("code");
        return new OidcClient<>(oidcConfiguration);
      }
    } catch (PersistenceException e) {
      log.debug("Defaulting to static configuration since runtime configuration is not available.");
      if (config.getString("yb.security.type", "").equals("OIDC")) {
        oidcConfiguration.setClientId(config.getString("yb.security.clientID", ""));
        oidcConfiguration.setSecret(config.getString("yb.security.secret", ""));
        oidcConfiguration.setScope(config.getString("yb.security.oidcScope", ""));
        oidcConfiguration.setDiscoveryURI(config.getString("yb.security.discoveryURI", ""));
        oidcConfiguration.setMaxClockSkew(3600);
        oidcConfiguration.setResponseType("code");
        return new OidcClient<>(oidcConfiguration);
      }
    }
    return new OidcClient<>(oidcConfiguration);
  }

  @Provides
  protected Config provideConfig(OidcClient<OidcProfile, OidcConfiguration> oidcClient) {
    final Clients clients =
        new Clients(
            String.format("%s/api/v1/callback", config.getString("yb.url", "")), oidcClient);
    final Config config = new Config(clients);
    config.setHttpActionAdapter(new PlatformHttpActionAdapter());
    return config;
  }
}
